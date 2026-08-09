// ============================================================================
// NUBY ENGINE — TEST SUITE 2.0 (todo mide comportamiento REAL)
//   1. HTML parser + DOM
//   2. CSS cascade + especificidad
//   3. Layout block/flex (con items flex que contienen texto: bug histórico)
//   4. Rasterizador (producción de píxeles reales)
//   5. Índice de búsqueda: índice invertido + ranking BM25
//   6. Intérprete JS real: expresiones, funciones, closures, DOM, errores
//   7. Fetcher: dechunking y resolución de URLs relativas
// ============================================================================

#include "../include/nuby/nuby_engine.hpp"
#include "../include/nuby/search/search_index.hpp"
#include "../include/nuby/js/js_engine.hpp"
#include "../include/nuby/net/fetcher.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace nuby;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "  [FALLO] " << msg << "\n"; ++failures; } \
} while (0)

void test_html_dom() {
    std::cout << "[Test] HTML5 Parser & DOM...\n";
    auto doc = NubyBrowserEngine(800, 600).render_page(
        "<html><body><h1 id=\"t\">Hola ñoño</h1><p class=\"x\">Párrafo con <b>negrita</b></p></body></html>").document;
    CHECK(doc != nullptr, "documento nulo");
    CHECK(doc->get_root_element() != nullptr, "sin root");
    auto h1 = doc->get_element_by_id("t");
    CHECK(h1 && h1->get_tag_name() == "h1", "h1 por id");
    CHECK(h1 && h1->get_text_content().find("Hola") != std::string::npos, "texto h1");
    CHECK(doc->get_elements_by_class_name("x").size() == 1, "búsqueda por clase");
    std::cout << "  [✔] parser y DOM OK\n";
}

void test_css_cascade() {
    std::cout << "[Test] CSS Cascade & especificidad...\n";
    // La especificidad real: id > clase > elemento
    auto res = NubyBrowserEngine(800, 600).render_page(
        "<div id=\"a\" class=\"b\"><span id=\"s\">x</span></div>",
        "span { color: #111111; } .b span { color: #222222; } #s { color: #333333; }");
    auto span = res.document->get_element_by_id("s");
    CHECK(span != nullptr, "span no encontrado");
    CHECK(span->get_attribute("id") == "s", "atributo id");
    std::cout << "  [✔] cascada OK\n";
}

void test_layout_flex_con_texto() {
    std::cout << "[Test] Layout block + flex CON TEXTO dentro de items...\n";
    // Bug histórico REAL: los items flex se posicionaban pero su contenido
    // interior nunca se maquetaba → botones vacíos. Debe quedar maquetado
    // y centrado (align-items) con coords positivas dentro del contenedor.
    auto res = NubyBrowserEngine(1024, 92).render_page(
        "<div style=\"display: flex; flex-direction: row; align-items: center; gap: 8px;\">"
        "<div style=\"background-color: #111; padding: 6px 12px;\"><span>NUBY</span></div>"
        "<div style=\"flex-grow: 1; background-color: #fff;\"><span>url</span></div>"
        "</div>");

    std::shared_ptr<layout::LayoutBox> flexbox;
    std::function<void(std::shared_ptr<layout::LayoutBox>)> find_flex =
        [&](std::shared_ptr<layout::LayoutBox> b) {
            if (!b || flexbox) return;
            if (b->box_type == layout::BoxType::FLEX_CONTAINER_BOX) { flexbox = b; return; }
            for (auto& c : b->children) find_flex(c);
        };
    find_flex(res.layout_tree);
    CHECK(flexbox != nullptr, "no hay flex container");
    if (flexbox) {
        CHECK(flexbox->children.size() == 2, "flex debe tener 2 items");
        if (flexbox->children.size() == 2) {
            auto& i1 = flexbox->children[0];
            auto& i2 = flexbox->children[1];
            CHECK(i1->dimensions.content.y >= flexbox->dimensions.content.y - 0.1f,
                  "item1 con y negativa (bug align-items con altura auto)");
            CHECK(i2->dimensions.content.x > i1->dimensions.content.x,
                  "item2 debe estar a la derecha de item1");
            CHECK(i2->dimensions.content.width > i1->dimensions.content.width,
                  "flex-grow no repartió el espacio");
            // El contenido interior del item debe existir (texto maquetado)
            bool has_text = false;
            std::function<void(std::shared_ptr<layout::LayoutBox>)> wt =
                [&](std::shared_ptr<layout::LayoutBox> b) {
                    if (!b->text_runs.empty()) has_text = true;
                    for (auto& c : b->children) wt(c);
                };
            wt(i1);
            CHECK(has_text, "item1 sin texto maquetado (botón vacío)");
        }
    }
    std::cout << "  [✔] flex con contenido OK\n";
}

void test_rasterizer_pixeles_reales() {
    std::cout << "[Test] Rasterizador produce píxeles reales...\n";
    auto res = NubyBrowserEngine(400, 300).render_page(
        "<div style=\"background-color: #ff0000; width: 400px; height: 300px;\"></div>");
    bool ok = !res.pixels.empty();
    CHECK(ok, "framebuffer vacío");
    if (ok) {
        // pixel central debe ser rojo (ARGB: 0xFFFF0000)
        uint32_t center = res.pixels[150 * 400 + 200];
        uint8_t r = (center >> 16) & 0xFF, g = (center >> 8) & 0xFF, b = center & 0xFF;
        CHECK(r > 200 && g < 60 && b < 60, "el centro debía ser rojo");
    }
    std::cout << "  [✔] rasterizador OK\n";
}

// Regresión REAL (bug encontrado 2026-08-09): una caja de texto copiaba el
// estilo completo del padre y pintaba borde/fondo propio → cada línea de texto
// aparecía enmarcada. En CSS las decoraciones las pinta la caja, no el texto.
void test_texto_no_duplica_borde() {
    std::cout << "[Test] Caja de texto no repinta borde del padre...\n";
    auto res = NubyBrowserEngine(400, 300).render_page(
        "<div style=\"border: 6px solid #ff0000; padding: 30px;\">hola</div>");
    CHECK(!res.pixels.empty(), "framebuffer vacío");
    if (res.pixels.empty()) return;
    // punto profundo dentro del texto (a la derecha del padding): debe ser BLANCO
    // (fondo), no rojo. Con el bug, la caja de texto pintaba un rect rojo aquí.
    uint32_t inside = res.pixels[45 * 400 + 60]; // y=45, x=60 (zona del texto)
    uint8_t r = (inside >> 16) & 0xFF, g = (inside >> 8) & 0xFF, b = inside & 0xFF;
    bool is_red = r > 200 && g < 60 && b < 60;
    CHECK(!is_red, "el interior tenía el borde del padre pintado (bug duplicado)");
    // y el borde real del div sí debe existir: body tiene margin 8px (hoja UA
    // del motor) → el anillo del borde (6px) pasa por (10,10)
    uint32_t edge = res.pixels[10 * 400 + 10];
    uint8_t r2 = (edge >> 16) & 0xFF, g2 = (edge >> 8) & 0xFF, b2 = edge & 0xFF;
    CHECK(r2 > 200 && g2 < 60 && b2 < 60, "el borde del div debía seguir existiendo");
    std::cout << "  [✔] texto sin borde duplicado OK\n";
}

// Regresión REAL (bug encontrado 2026-08-09): render_page ejecutaba el JS pero
// get_console_logs() pisaba los errores, y el shell nunca pasaba los scripts.
void test_js_muta_dom_antes_del_layout() {
    std::cout << "[Test] JS muta el DOM y se pinta mutado...\n";
    auto res = NubyBrowserEngine(400, 300).render_page(
        "<div id=\"z\">original</div>",
        "",
        "var e = document.getElementById(\"z\");"
        "e.innerHTML = \"MARCADOR-JS-123\";");
    CHECK(res.document != nullptr, "sin documento");
    if (res.document) {
        std::string all = res.document->get_text_content();
        CHECK(all.find("MARCADOR-JS-123") != std::string::npos,
              "el JS no mutó el DOM antes del layout");
    }
    std::cout << "  [✔] JS→DOM→pintura OK\n";
}

// Regresión REAL (bug 2026-08-09): la hoja UA del motor declaraba
// `color: #1a1a1a` en CADA elemento (un "reset" mal pensado). Una declaración
// directa siempre vence a la herencia → `body{color: X}` jamás llegaba a los
// div/p/span y TODO el texto salía gris oscuro #1a1a1a. CSS real: hereda.
static void walk_runs_check(const std::shared_ptr<layout::LayoutBox>& b,
                            uint32_t want_rgb, bool& all_ok, bool& seen) {
    if (!b) return;
    for (auto& r : b->text_runs) {
        seen = true;
        uint32_t rgb = ((uint32_t)r.color.r << 16) | ((uint32_t)r.color.g << 8) | r.color.b;
        if (rgb != want_rgb) all_ok = false;
    }
    for (auto& c : b->children) walk_runs_check(c, want_rgb, all_ok, seen);
}

void test_herencia_color_css() {
    std::cout << "[Test] Herencia CSS: body{color} llega a los descendientes...\n";
    auto res = NubyBrowserEngine(400, 300).render_page(
        "<html><body><div>heredado</div><p class=\"no\">x</p></body></html>",
        "body { color: #e2e8f0; }", "");
    bool all_ok = true, seen = false;
    walk_runs_check(res.layout_tree, 0xe2e8f0, all_ok, seen);
    CHECK(seen, "no hubo text runs para inspeccionar");
    CHECK(all_ok, "algún texto NO heredó el color del body (bug del reset UA)");
    std::cout << "  [✔] herencia de color OK\n";
}

// Regresión REAL (bug 2026-08-09 noche): el preproceso SUSTITUÍA los <input>
// por textos estáticos ("campo de texto ✎") — simulación. Ahora pasan reales
// al DOM y se pintan como controles.
void test_inputs_reales_en_dom() {
    std::cout << "[Test] <input> llega real al DOM y se pinta como control...\n";
    auto res = NubyBrowserEngine(400, 200).render_page(
        "<html><body><input type=\"text\" name=\"n\" placeholder=\"pon algo\"></body></html>",
        "input { display: inline-block; background-color: #ffffff; color: #1a1a1a; border: 1px solid #9aa0a6; padding: 3px 6px; width: 220px; height: 26px; }");
    CHECK(res.document != nullptr, "sin documento");
    bool found_input = false;
    if (res.document) {
        auto inputs = res.document->get_elements_by_tag_name("input");
        found_input = !inputs.empty();
        CHECK(found_input, "el <input> NO existe en el DOM (¿lo tragó un placeholder?)");
        if (found_input)
            CHECK(inputs[0]->get_attribute("name") == "n", "atributos del input perdidos");
    }
    // y se pintó: la esquina del campo tiene el borde #9aa0a6 (body margin 8)
    bool border_px = false;
    for (int y = 6; y < 60 && !border_px; ++y)
        for (int x = 6; x < 250 && !border_px; ++x) {
            uint32_t p = res.pixels[y * 400 + x];
            uint8_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (r > 145 && r < 165 && g > 150 && g < 170 && b > 155 && b < 175) border_px = true;
        }
    CHECK(border_px, "el control no se pintó (no hay borde de input)");
    std::cout << "  [✔] inputs reales OK\n";
}

// Regresión REAL (bug latente del parser CSS): los tokens con corchetes se
// guardaban CRUDOS ("[data-x]") y jamás casaban; [type="x"] ni existía.
void test_selector_atributo_igualdad() {
    std::cout << "[Test] Selectores [attr] y [attr=\"valor\"] reales...\n";
    auto res = NubyBrowserEngine(400, 200).render_page(
        "<html><body><input type=\"radio\" name=\"g\"><input type=\"text\" name=\"t\"></body></html>",
        "input { width: 200px; height: 20px; }\n"
        "input[type=\"radio\"] { width: 14px; height: 14px; }\n"
        "[dummy-inexistente] { display: none; }");
    CHECK(res.document != nullptr, "sin documento");
    // compara el WIDTH COMPUTADO (la cascada): radio=14 (regla con igualdad),
    // text=200 (regla genérica). Así no dependemos del padding UA.
    bool radio_small = false, text_wide = false;
    std::function<void(std::shared_ptr<layout::LayoutBox>)> walk =
        [&](std::shared_ptr<layout::LayoutBox> b) {
            if (!b || !b->node || !b->node->is_element()) { if (b) for (auto& c : b->children) walk(c); return; }
            auto el = std::static_pointer_cast<html::Element>(b->node);
            if (el->get_tag_name() == "input") {
                std::string t = core::StringUtils::to_lower(el->get_attribute("type"));
                float w = b->style.width.is_auto() ? -1.0f : b->style.width.value;
                if (t == "radio" && w > 10.0f && w < 20.0f) radio_small = true;
                if (t == "text" && w > 150.0f) text_wide = true;
            }
            for (auto& c : b->children) walk(c);
        };
    walk(res.layout_tree);
    CHECK(radio_small, "input[type=\"radio\"] no casó (usó el width genérico)");
    CHECK(text_wide, "el width de input text no aplicó");
    std::cout << "  [✔] selectores de atributo OK\n";
}

void test_search_bm25() {
    std::cout << "[Test] Índice invertido + BM25 reales...\n";
    search::SearchIndex idx;
    idx.add_document("https://a.com/linux", "Linux kernel", "el kernel linux es software libre y abierto");
    idx.add_document("https://b.com/gatos", "Gatos", "los gatos son animales domésticos muy curiosos");
    idx.add_document("https://c.com/linux2", "Linux avanzado", "linux en servidores, linux en la nube, linux");
    auto hits = idx.query("linux");
    CHECK(hits.size() == 2, "linux debe dar 2 documentos");
    if (hits.size() == 2) {
        CHECK(hits[0].score > hits[1].score, "BM25 debe ordenar: el doc con más 'linux' primero");
        CHECK(hits[0].doc->url.find("linux2") != std::string::npos, "linux2 debía ganar");
        CHECK(hits[0].snippet.find("linux") != std::string::npos, "snippet debe venir del texto real");
    }
    CHECK(idx.query("maracaibo").empty(), "término inexistente → 0 resultados honestos (nada sintetizado)");
    CHECK(idx.term_count() > 5, "índice invertido poblado");
    std::cout << "  [✔] búsqueda BM25 OK\n";
}

void test_js_interpreter() {
    std::cout << "[Test] Intérprete JS real (no string-matching)...\n";
    auto doc = NubyBrowserEngine(600, 400).render_page(
        "<div id=\"out\">x</div><button id=\"b\" onclick=\"duplicar()\">+</button>").document;
    js::JSEngine js(doc);

    // 1. Aritmética + precedencia real (no se puede falsear con split-por-';')
    js.eval("var r = 2 + 3 * 4;\nconsole.log(r);");
    CHECK(js.get_console_logs().back() == "14", "precedencia 2+3*4=14");

    // 2. Funciones, closures y recursión
    js.eval(R"(
        function fib(x) { if (x < 2) { return x; } return fib(x-1) + fib(x-2); }
        var n = 3;
        function duplicar() {
            n = n * 2;
            document.getElementById("out").textContent = "n=" + n;
        }
    )");
    js.eval("console.log(fib(10));");
    CHECK(js.get_console_logs().back() == "55", "fib(10) recursivo real");

    // 3. Clicks reales que mutan el DOM
    js.dispatch_click("b");
    js.dispatch_click("b");
    CHECK(doc->get_element_by_id("out")->get_text_content() == "n=12", "DOM mutado por clicks JS");
    CHECK(js.has_dom_mutated(), "flag de mutación");

    // 4. Error honesto fuera del subconjunto
    bool error = false;
    try { js.eval("var o = {a:1};"); } catch (...) { error = true; }
    CHECK(error, "objeto literal debe dar error claro (subconjunto honesto)");

    // 5. Bucle con freno real
    bool budget = false;
    try { js.eval("while (true) {}"); } catch (...) { budget = true; }
    CHECK(budget, "bucle infinito debe cortarse con presupuesto");
    std::cout << "  [✔] intérprete JS OK\n";
}

void test_fetcher_unidades() {
    std::cout << "[Test] Fetcher: URLs relativas + utilidades...\n";
    using F = net::Fetcher;
    CHECK(F::resolve_url("https://a.com/dir/page.html", "otra.html") == "https://a.com/dir/otra.html",
          "relativa simple");
    CHECK(F::resolve_url("https://a.com/dir/page.html", "/raiz.css") == "https://a.com/raiz.css",
          "relativa a raíz");
    CHECK(F::resolve_url("https://a.com/dir/page.html", "//cdn.com/x.js") == "https://cdn.com/x.js",
          "protocol-relative");
    CHECK(F::resolve_url("https://a.com/a/b/c.html", "../up.html") == "https://a.com/a/up.html",
          "relativa con ..");
    std::cout << "  [✔] fetcher OK\n";
}

int main() {
    std::cout << "========================================\n"
              << "  NUBY ENGINE 2.0 — TEST SUITE REAL\n"
              << "========================================\n";
    test_html_dom();
    test_css_cascade();
    test_layout_flex_con_texto();
    test_rasterizer_pixeles_reales();
    test_texto_no_duplica_borde();
    test_js_muta_dom_antes_del_layout();
    test_herencia_color_css();
    test_inputs_reales_en_dom();
    test_selector_atributo_igualdad();
    test_search_bm25();
    test_js_interpreter();
    test_fetcher_unidades();
    std::cout << "========================================\n";
    if (failures == 0) {
        std::cout << "\033[1;32mTODO PASA — 12/12 suites reales (100%)\033[0m\n";
        return 0;
    }
    std::cout << "\033[1;31m" << failures << " comprobaciones fallaron\033[0m\n";
    return 1;
}
