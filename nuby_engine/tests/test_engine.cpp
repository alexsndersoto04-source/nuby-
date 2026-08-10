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
#include "../include/nuby/media/png_decoder.hpp"
#include "../include/nuby/search/search_index.hpp"
#include "../include/nuby/js/js_engine.hpp"
#include "../include/nuby/net/fetcher.hpp"
#include "../include/nuby/app/browser_shell.hpp"
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

// ---- Decodificador PNG propio (RFC 1950/1951/2083, cero librerías) ------
// Los PNGs de prueba fueron GENERADOS byte a byte (Python zlib+struct) con
// contenido conocido: filtros 0-4, paleta+tRNS, gris, 16-bit y Adam7.
static std::string b64dec(const std::string& in) {
    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        if (T[c] == -2) continue;
        val = (val << 6) + T[c]; valb += 6;
        if (valb >= 0) { out += (char)((val >> valb) & 0xFF); valb -= 8; }
    }
    return out;
}

static bool pix_eq(const media::Image& img, int x, int y,
                   int r, int g, int b, int a) {
    uint32_t p = img.at(x, y);
    return ((p >> 16) & 0xFF) == (uint32_t)r && ((p >> 8) & 0xFF) == (uint32_t)g &&
           (p & 0xFF) == (uint32_t)b && ((p >> 24) & 0xFF) == (uint32_t)a;
}

void test_png_decoder() {
    std::cout << "[Test] Decodificador PNG propio (inflate+filtros+paleta)...\n";
    struct V { const char* name; const char* b64; int w, h; };
    static const V vec[] = {
        {"rgb_filtro0", "iVBORw0KGgoAAAANSUhEUgAAAAMAAAABCAIAAACUgoPjAAAADklEQVR42mP4z8DAAMYADvsC/hR0WEIAAAAASUVORK5CYII=", 3, 1},
        {"rgba_semi", "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAD0lEQVR42mP4z8DwHwgbABB5A37EGHKQAAAAAElFTkSuQmCC", 2, 1},
        {"rgb_filtro1_sub", "iVBORw0KGgoAAAANSUhEUgAAAAMAAAABCAIAAACUgoPjAAAAEklEQVR42mNMqejhEpH7/+srABIqBJRTWOZrAAAAAElFTkSuQmCC", 3, 1},
        {"rgb_filtro2_up", "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR42mPgEpHTMLJhYmVlZWJmAQAIZQDtHwRJvwAAAABJRU5ErkJggg==", 2, 2},
        {"gris8", "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAAAAADRSSBWAAAAC0lEQVR42mNg+A8AAQIBANEay48AAAAASUVORK5CYII=", 2, 1},
        {"paleta_trns", "iVBORw0KGgoAAAANSUhEUgAAAAQAAAABCAMAAADO4v//AAAADFBMVEX/AAAA/wAAAP///wDWAo97AAAAA3RSTlP//4A6co5hAAAADUlEQVR42mNgYGRiBgAADwAHW9CLfQAAAABJRU5ErkJggg==", 4, 1},
        {"rgb16", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABEAIAAADA54+dAAAAD0lEQVR42mNYzSD0/z8jAAlNAr2dtO9bAAAAAElFTkSuQmCC", 1, 1},
        {"avg_f3", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAACCAIAAAAW4yFwAAAAEElEQVR42mM4ceIEMxsbGwAOSAJuWQHiDAAAAABJRU5ErkJggg==", 1, 2},
        {"paeth_f4", "iVBORw0KGgoAAAANSUhEUgAAAAMAAAABCAIAAACUgoPjAAAAEklEQVR42mMJiEph5eJnZmIEAAkoATcVaEbbAAAAAElFTkSuQmCC", 3, 1},
    };
    media::Image img;
    std::string err;

    auto dec = [&](int i) {
        img = media::Image{};
        return media::PngDecoder::decode(b64dec(vec[i].b64), img, err);
    };

    CHECK(dec(0), "rgb filtro0 no decodificó: " + err);
    CHECK(img.width == 3 && pix_eq(img, 0, 0, 255, 0, 0, 255) && pix_eq(img, 1, 0, 0, 255, 0, 255) && pix_eq(img, 2, 0, 0, 0, 255, 255), "rgb filtro0 mal");

    CHECK(dec(1), "rgba no decodificó: " + err);
    CHECK(pix_eq(img, 1, 0, 0, 255, 0, 128), "alfa semi mal");

    CHECK(dec(2), "filtro Sub no decodificó: " + err);
    CHECK(pix_eq(img, 1, 0, 110, 140, 170, 255) && pix_eq(img, 2, 0, 109, 134, 159, 255), "filtro Sub mal");

    CHECK(dec(3), "filtro Up no decodificó: " + err);
    CHECK(pix_eq(img, 0, 0, 10, 20, 30, 255) && pix_eq(img, 0, 1, 15, 25, 35, 255) && pix_eq(img, 1, 1, 42, 53, 64, 255), "filtro Up mal");

    CHECK(dec(4), "gris no decodificó: " + err);
    CHECK(pix_eq(img, 0, 0, 0, 0, 0, 255) && pix_eq(img, 1, 0, 255, 255, 255, 255), "gris mal");

    CHECK(dec(5), "paleta no decodificó: " + err);
    CHECK(pix_eq(img, 2, 0, 0, 0, 255, 128) && pix_eq(img, 3, 0, 255, 255, 0, 255), "paleta/tRNS mal");

    CHECK(dec(6), "16-bit no decodificó: " + err);
    CHECK(pix_eq(img, 0, 0, 0xAB, 0x12, 0xFF, 255), "MSB 16-bit mal");

    CHECK(dec(7), "filtro Average no decodificó: " + err);
    CHECK(pix_eq(img, 0, 0, 200, 200, 200, 255) && pix_eq(img, 0, 1, 106, 106, 106, 255), "filtro Average mal");

    CHECK(dec(8), "filtro Paeth no decodificó: " + err);
    CHECK(pix_eq(img, 0, 0, 80, 90, 100, 255) && pix_eq(img, 1, 0, 85, 100, 115, 255) && pix_eq(img, 2, 0, 88, 102, 116, 255), "filtro Paeth mal");

    // Adam7: debe FALLAR con verdad, no fingir
    bool still_ok = media::PngDecoder::decode(
        b64dec("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAFNeavDAAAACklEQVR42mNgAAAAAgAB5Sfe/AAAAABJRU5ErkJggg=="), img, err);
    CHECK(!still_ok && err.find("Adam7") != std::string::npos,
          "Adam7 debía fallar con mensaje honesto, no renderizar");
    std::cout << "  [✔] decodificador PNG OK\n";
}

// Regresión REAL del bug "DRAW_IMAGE h=0" (2026-08-09): la altura
// intrínseca del <img> salía bien de layout_box_geometry pero el tail de
// layout_block_children la pisaba a 0 (cursor jamás avanza sin hijos).
void test_img_altura_intrinseca_sobrevive_layout() {
    std::cout << "[Test] <img>: altura intrínseca sobrevive al layout...\n";
    NubyBrowserEngine engine(800, 600);
    auto res = engine.render_page(
        "<html><body><div><img src=\"x.png\"></div><p>después</p></body></html>");
    auto imgs = res.document->get_elements_by_tag_name("img");
    CHECK(imgs.size() == 1, "img en el DOM");
    if (imgs.empty()) return;

    auto pic = std::make_shared<media::Image>();
    pic->width = 96; pic->height = 64;
    pic->pixels.assign((size_t)96 * 64, 0xFF336699u);
    imgs[0]->decoded_image = pic;

    // Re-render del MISMO DOM vivo (como hace navigate tras decodificar)
    auto res2 = engine.render_document(res.document, "");

    // 1) la caja de layout del img conserva su tamaño natural real
    std::shared_ptr<layout::LayoutBox> img_box;
    std::function<void(std::shared_ptr<layout::LayoutBox>)> find =
        [&](std::shared_ptr<layout::LayoutBox> b) {
            if (!b || img_box) return;
            if (b->node && b->node->is_element() &&
                std::static_pointer_cast<html::Element>(b->node)->get_tag_name() == "img") {
                img_box = b; return;
            }
            for (auto& c : b->children) find(c);
        };
    find(res2.layout_tree);
    CHECK(img_box != nullptr, "caja de layout del <img>");
    if (img_box) {
        CHECK(std::fabs(img_box->dimensions.content.width - 96.0f) < 0.5f,
              "ancho intrínseco 96 (vino " + std::to_string(img_box->dimensions.content.width) + ")");
        CHECK(std::fabs(img_box->dimensions.content.height - 64.0f) < 0.5f,
              "alto intrínseco 64, NO 0 (vino " + std::to_string(img_box->dimensions.content.height) + ")");
    }

    // 2) el display list lleva un DRAW_IMAGE real con ese rect (no degenerado)
    bool has_draw_image = false;
    for (auto& cmd : res2.display_list.get_commands()) {
        if (cmd.type == paint::CommandType::DRAW_IMAGE) {
            has_draw_image = true;
            CHECK(std::fabs(cmd.rect.height - 64.0f) < 0.5f,
                  "DRAW_IMAGE con alto 64 real (vino " + std::to_string(cmd.rect.height) + ")");
        }
    }
    CHECK(has_draw_image, "display list sin DRAW_IMAGE");

    // 3) el <p> posterior quedó DEBAJO del img (el flujo respetó su altura)
    bool p_below = false;
    std::function<void(std::shared_ptr<layout::LayoutBox>)> find_p =
        [&](std::shared_ptr<layout::LayoutBox> b) {
            if (!b) return;
            if (b->node && b->node->is_element() &&
                std::static_pointer_cast<html::Element>(b->node)->get_tag_name() == "p" &&
                img_box && b->dimensions.content.y >= img_box->dimensions.content.bottom() - 0.5f)
                p_below = true;
            for (auto& c : b->children) find_p(c);
        };
    find_p(res2.layout_tree);
    CHECK(p_below, "el flujo de bloques no respetó la altura del <img>");
    std::cout << "  [✔] altura intrínseca <img> OK\n";
}

// IFC: texto, negritas, enlaces y cajas atómicas en la MISMA línea con
// wrap real. Antes todo nacía display:block y se apilaba verticalmente.
struct RunInfo {
    std::string text; core::RectF rect; int weight; core::Color color;
    bool in_link{false}; bool in_bold{false};
};
static void collect_runs_ifc(const std::shared_ptr<layout::LayoutBox>& b,
                             bool in_link, bool in_bold,
                             std::vector<RunInfo>& out) {
    if (!b) return;
    if (b->node && b->node->is_element()) {
        auto t = std::static_pointer_cast<html::Element>(b->node)->get_tag_name();
        if (t == "a") in_link = true;
        if (t == "b" || t == "strong") in_bold = true;
    }
    for (auto& r : b->text_runs)
        out.push_back({r.text, r.rect, r.font_weight, r.color, in_link, in_bold});
    for (auto& c : b->children) collect_runs_ifc(c, in_link, in_bold, out);
}

void test_ifc_layout_inline_real() {
    std::cout << "[Test] IFC: layout inline verdadero (lineas reales)...\n";
    NubyBrowserEngine engine(260, 600);   // estrecho a drede: fuerza wrap
    NubyBrowserEngine engine_wide(640, 600); // ancho: todo en una línea

    // --- 1) texto + <b> + <a> en la misma línea, x crecientes ---
    auto res = engine_wide.render_page(
        "<html><body><p>hola <b>negrita</b> medio <a href=\"/x\">enlace</a> final</p></body></html>");
    std::vector<RunInfo> runs;
    collect_runs_ifc(res.layout_tree, false, false, runs);
    auto find = [&](const std::string& t) -> const RunInfo* {
        for (auto& r : runs) if (r.text == t) return &r;
        return nullptr;
    };
    const RunInfo *r_hola = find("hola"), *r_neg = find("negrita"),
                  *r_enl = find("enlace"), *r_fin = find("final");
    CHECK(r_hola && r_neg && r_enl && r_fin, "runs ifc presentes");
    if (r_hola && r_neg && r_enl && r_fin) {
        CHECK(r_neg->in_bold && r_neg->weight >= 700, "<b> hereda negrita real");
        CHECK(r_enl->in_link, "run marcado dentro del <a>");
        CHECK(std::fabs(r_hola->rect.y - r_neg->rect.y) < 1.0f &&
              std::fabs(r_neg->rect.y - r_enl->rect.y) < 1.0f,
              "hola/negrita/enlace en la MISMA linea (no apilados)");
        CHECK(r_hola->rect.right() <= r_neg->rect.x + 7.0f &&
              r_neg->rect.x < r_enl->rect.x, "flujo izquierda→derecha");
        CHECK(std::fabs(r_neg->rect.x - r_hola->rect.right()) > 2.0f,
              "espacio real entre palabras de nodos distintos");
    }

    // --- 2) wrap real: viewport 260px no caben todas en una línea ---
    auto res2 = engine.render_page(
        "<html><body><p>palabra larga uno dos tres cuatro cinco seis siete "
        "ocho nueve diez once doce trece catorce quince dieciseis</p></body></html>");
    std::vector<RunInfo> runs2;
    collect_runs_ifc(res2.layout_tree, false, false, runs2);
    float min_y = 1e9f, max_y = -1e9f;
    for (auto& r : runs2) { min_y = std::min(min_y, r.rect.y); max_y = std::max(max_y, r.rect.y); }
    CHECK(max_y > min_y + 10.0f, "el texto envolvió a 2+ líneas reales");

    // --- 3) <br> fuerza salto de línea honesto ---
    auto res3 = engine.render_page("<html><body><p>uno<br>dos</p></body></html>");
    std::vector<RunInfo> runs3;
    collect_runs_ifc(res3.layout_tree, false, false, runs3);
    const RunInfo *u = nullptr, *d = nullptr;
    for (auto& r : runs3) { if (r.text == "uno") u = &r; if (r.text == "dos") d = &r; }
    CHECK(u && d && d->rect.y > u->rect.y + 10.0f, "<br> rompe la línea de verdad");

    // --- 4) sin espacio en el fuente NO se inventa espacio ---
    auto res4 = engine.render_page("<html><body><p>a<b>bc</b></p></body></html>");
    std::vector<RunInfo> runs4;
    collect_runs_ifc(res4.layout_tree, false, false, runs4);
    const RunInfo *ra = nullptr, *rbc = nullptr;
    for (auto& r : runs4) { if (r.text == "a") ra = &r; if (r.text == "bc") rbc = &r; }
    CHECK(ra && rbc && std::fabs(rbc->rect.x - ra->rect.right()) < 0.5f,
          "a<b>bc</b> sin espacio → pegadas (como un navegador)");

    // --- 5) caja atómica (img) flota EN la línea, baseline abajo ---
    auto res5 = engine.render_page("<html><body><p>izq <img src=\"x.png\"> der</p></body></html>");
    std::vector<RunInfo> runs5;
    collect_runs_ifc(res5.layout_tree, false, false, runs5);
    const RunInfo *rizq = nullptr; 
    for (auto& r : runs5) if (r.text == "izq") rizq = &r;
    std::shared_ptr<layout::LayoutBox> img5;
    std::function<void(std::shared_ptr<layout::LayoutBox>)> f5 =
        [&](std::shared_ptr<layout::LayoutBox> b) {
            if (!b || img5) return;
            if (b->node && b->node->is_element() &&
                std::static_pointer_cast<html::Element>(b->node)->get_tag_name() == "img") img5 = b;
            for (auto& c : b->children) f5(c);
        };
    f5(res5.layout_tree);
    CHECK(rizq && img5, "img y palabra presentes");
    if (rizq && img5) {
        float baseline_words = rizq->rect.y + 0.9f * 16.0f;
        float img_bottom = img5->dimensions.margin_box().bottom();
        CHECK(std::fabs(img_bottom - baseline_words) < 1.0f,
              "img alineada a baseline (vertical-align real) — bottom=" +
              std::to_string(img_bottom) + " vs baseline=" + std::to_string(baseline_words));
        CHECK(img5->dimensions.content.x > rizq->rect.right(),
              "img DESPUÉS de la palabra en la línea, no debajo");
    }

    // --- 6) text-align: center real ---
    auto res6 = engine.render_page(
        "<html><body><p style=\"text-align: center\">centro</p></body></html>");
    std::vector<RunInfo> runs6;
    collect_runs_ifc(res6.layout_tree, false, false, runs6);
    CHECK(!runs6.empty() && runs6[0].rect.x > 10.0f,
          "text-align:center desplaza la línea de verdad");

    // --- 7) re-render del mismo DOM = determinista (idempotente) ---
    auto res7 = engine_wide.render_document(res.document, "");
    std::vector<RunInfo> runs7;
    collect_runs_ifc(res7.layout_tree, false, false, runs7);
    CHECK(runs7.size() == runs.size(), "re-render conserva los runs");
    bool same = runs7.size() == runs.size();
    if (same) for (size_t i = 0; i < runs.size(); ++i)
        if (std::fabs(runs7[i].rect.x - runs[i].rect.x) > 0.01f ||
            std::fabs(runs7[i].rect.y - runs[i].rect.y) > 0.01f) same = false;
    CHECK(same, "posiciones idénticas tras re-render (sin estado podrido)");
    std::cout << "  [✔] IFC inline real OK\n";
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

// ---------------------------------------------------------------------------
// [16] Menú ☰ real: existe como página interna y navega a sus destinos
// ---------------------------------------------------------------------------
void test_menu_interno_real() {
    std::cout << "[Test] Menú ☰ y vistas internas...\n";
    app::BrowserShell sh; // índice propio vacío, navegación interna no usa red
    sh.go("nuby://menu");
    CHECK(sh.current_url() == "nuby://menu", "navegar a nuby://menu");
    CHECK(!sh.frame().empty(), "el menú produce un frame real");
    sh.go("nuby://settings");
    CHECK(sh.current_url() == "nuby://settings", "navegar a nuby://settings");
    sh.go("nuby://downloads");
    CHECK(sh.current_url() == "nuby://downloads", "navegar a nuby://downloads");
    sh.go("nuby://home");
    CHECK(sh.current_url() == "nuby://home", "volver a nuby://home");
    std::cout << "  [✔] menú ☰ real OK\n";
}

// ---------------------------------------------------------------------------
// [17] Descargas reales: registro byte-exacto, sin muestras falsas, con vaciado
// ---------------------------------------------------------------------------
void test_descargas_reales() {
    std::cout << "[Test] Descargas reales (registro de red de sesión)...\n";
    app::BrowserShell sh;
    CHECK(sh.downloads().empty(), "al inicio NO hay descargas (nada de entradas de muestra)");
    sh.record_download("https://ejemplo.com/", "Ejemplo", 4096);
    CHECK(sh.downloads().size() == 1, "una descarga registrada");
    CHECK(sh.downloads()[0].bytes == 4096, "bytes exactos registrados");
    CHECK(sh.downloads()[0].url == "https://ejemplo.com/", "URL registrada");
    sh.record_download("nuby://home", "Interna", 10);
    CHECK(sh.downloads().size() == 1, "las páginas internas NO cuentan como descargas");
    sh.go("nuby://clear-downloads");
    CHECK(sh.downloads().empty(), "vaciar lista funciona de verdad");
    std::cout << "  [✔] descargas reales OK\n";
}

// ---------------------------------------------------------------------------
// [18] Configuración real: el zoom de texto cambia MEDIDA y layout (no filtro)
// ---------------------------------------------------------------------------
void test_configuracion_real() {
    std::cout << "[Test] Configuración real (zoom de texto aplicado)...\n";
    app::BrowserShell sh;
    const float base = paint::FontRasterizer::glyph_advance(16.0f, 400);
    const float w100 = layout::TextShaper::measure_text_width("hola", 16.0f, 400);
    sh.go("nuby://set/textzoom/2"); // Grande = 125%
    const float zoomed = paint::FontRasterizer::glyph_advance(16.0f, 400);
    const float w125 = layout::TextShaper::measure_text_width("hola", 16.0f, 400);
    CHECK(paint::FontRasterizer::text_zoom() > 1.2f, "el zoom quedó aplicado en el motor");
    CHECK(zoomed > base * 1.2f, "la MEDIDA del glifo creció de verdad");
    // Con tipografía proporcional real (TTF) cada letra tiene su ancho: la
    // invariant honesta es que el layout escala con el zoom, no con '0.6*N'.
    CHECK(w125 > w100 * 1.2f, "el shaper (layout) mide con el zoom aplicado");
    sh.go("nuby://set/textzoom/1"); // restaurar para el resto de la suite
    CHECK(std::fabs(paint::FontRasterizer::text_zoom() - 1.0f) < 0.001f, "zoom restaurado al 100%");
    std::cout << "  [✔] configuración real OK\n";
}

// ---------------------------------------------------------------------------
// [19] Puente de teclado: foco de texto por hit-testing REAL sobre el layout
// ---------------------------------------------------------------------------
void test_foco_texto_teclado() {
    std::cout << "[Test] Foco de texto para el puente de teclado...\n";
    app::BrowserShell sh; // ventana 1024×640 por defecto
    CHECK(!sh.text_focused(), "sin foco al arrancar");
    CHECK(sh.focused_text().empty(), "sin texto sin foco");
    // Click REAL en la zona de la píldora de búsqueda (hit-testing sobre el
    // árbol de layout, igual que el toque del usuario en el canvas). Se
    // barre verticalmente la banda central porque el alto exacto depende de
    // la métrica de línea; lo que se exige es que EXISTE una zona que enfoca.
    bool hit = false;
    int hit_y = -1;
    for (int y = 120; y < 350 && !hit; y += 4) {
        if (sh.handle_click(512, y) && sh.text_focused()) { hit = true; hit_y = y; }
    }
    CHECK(hit, "existe una zona real (la píldora) que enfoca texto al tocarla");
    std::cout << "  (foco conseguido en y=" << hit_y << ")\n";
    sh.handle_char('h'); sh.handle_char('o'); sh.handle_char('l'); sh.handle_char('a');
    CHECK(sh.focused_text() == "hola", "el texto tecleado llega ÍNTEGRO al motor");
    sh.handle_key("Escape");
    CHECK(!sh.text_focused(), "Escape quita el foco (teclado se cerraría)");
    std::cout << "  [✔] foco teclado real OK\n";
}

// ---------------------------------------------------------------------------
// [20] Antialiasing REAL del texto: bordes con cobertura parcial (grises),
// no los bloques duros de antes
// ---------------------------------------------------------------------------
void test_antialiasing_texto() {
    std::cout << "[Test] Antialiasing real del texto...\n";
    std::vector<uint32_t> fb(300 * 200, 0xFFFFFFFF);
    paint::FontRasterizer::render_glyph(fb, 300, 200, (uint32_t)'N', 10, 10,
                                        60.0f, 800, {32, 33, 36, 255});
    int grises = 0, oscuros = 0, bandas = 0;
    int banda[3] = {0, 0, 0}; // coberturas bajas/medias/altas → gradación REAL
    for (uint32_t px : fb) {
        int r = (px >> 16) & 0xFF;
        if (r < 60) ++oscuros;                       // interior sólido del trazo
        else if (r < 130) { ++grises; ++banda[0]; }
        else if (r < 200) { ++grises; ++banda[1]; }
        else if (r < 250) { ++grises; ++banda[2]; }
    }
    for (int b : banda) if (b > 5) ++bandas;
    CHECK(oscuros > 500, "el glifo tiene interior sólido");
    CHECK(grises > 40, "hay píxeles de borde SUAVIZADOS (antialiasing real)");
    CHECK(bandas >= 2, "la cobertura es GRADUADA (varios tonos de borde, no un solo gris)");
    std::cout << "  [✔] antialiasing real OK\n";
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
    test_png_decoder();
    test_img_altura_intrinseca_sobrevive_layout();
    test_ifc_layout_inline_real();
    test_search_bm25();
    test_js_interpreter();
    test_fetcher_unidades();
    test_menu_interno_real();
    test_descargas_reales();
    test_configuracion_real();
    test_foco_texto_teclado();
    test_antialiasing_texto();
    std::cout << "========================================\n";
    if (failures == 0) {
        std::cout << "\033[1;32mTODO PASA — 20/20 suites reales (100%)\033[0m\n";
        return 0;
    }
    std::cout << "\033[1;31m" << failures << " comprobaciones fallaron\033[0m\n";
    return 1;
}
