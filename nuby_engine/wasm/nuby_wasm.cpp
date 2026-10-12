// ============================================================================
// NUBY EN WEBASSEMBLY — el motor completo dentro del navegador del visitante.
//
// Qué es esto: el MISMO motor (parser HTML, CSS, layout, rasterizador, fuentes
// TrueType, buscador BM25 y la interfaz del navegador BrowserShell) compilado
// a wasm32-wasi. Aquí solo vive el puente: funciones C que la página anfitriona
// llama para pedir el frame, mandar clics/teclas y leer el estado.
//
// Qué NO hace: no inventa píxeles ni resultados. Cada frame sale del
// rasterizador del motor; cada resultado de búsqueda sale del índice BM25 real.
//
// La red (pestañas de sitio web) la presta el anfitrión con su propio fetch:
// ver net/fetcher.hpp (rama __wasi__). El resto es el motor sin cambios.
// ============================================================================

#include "../include/nuby/app/browser_shell.hpp"
#include "../include/nuby/server/rle_codec.hpp"
#include "../include/nuby/search/search_index.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

struct WasmBrowser {
    std::shared_ptr<nuby::search::SearchIndex> index;
    std::unique_ptr<nuby::app::BrowserShell> shell;
    std::vector<unsigned char> rle;
    std::string text;
    bool alive{false};
};

WasmBrowser& browser() {
    static WasmBrowser b;
    return b;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += " ";    break;
            case '\r': o += " ";    break;
            case '\t': o += " ";    break;
            default:
                if ((unsigned char)c < 0x20) o += ' ';
                else o += c;
        }
    }
    return o;
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------

// Reserva memoria dentro del wasm (la usa el anfitrión para mandar datos:
// el fichero TSV del índice, por ejemplo).
void* nuby_alloc(int bytes) {
    return std::malloc((size_t)bytes);
}

// Arranca el navegador. Carga el índice desde el sistema de archivos virtual
// (que el anfitrión rellena con el TSV real del rastreo) y crea la ventana
// virtual w×h. Devuelve el número de documentos indexados.
int nuby_boot(const char* tsv_path, int w, int h) {
    WasmBrowser& b = browser();
    b.index = std::make_shared<nuby::search::SearchIndex>();
    size_t loaded = b.index->load(tsv_path ? tsv_path : "data/crawl_pages.tsv");
    b.shell = std::make_unique<nuby::app::BrowserShell>(b.index);
    if (tsv_path) b.shell->set_data_path(tsv_path);
    b.shell->set_viewport(w, h);
    b.shell->render_current_mode();
    b.alive = true;
    return (int)loaded;
}

int nuby_documents() {
    WasmBrowser& b = browser();
    return b.index ? (int)b.index->document_count() : 0;
}

int nuby_terms() {
    WasmBrowser& b = browser();
    return b.index ? (int)b.index->term_count() : 0;
}

int nuby_width()  { return browser().alive ? browser().shell->width()  : 0; }
int nuby_height() { return browser().alive ? browser().shell->height() : 0; }

// ---------------------------------------------------------------------------
//  Entrada real (la misma que recibe el servidor nativo por /event)
// ---------------------------------------------------------------------------

int nuby_click(int x, int y) {
    WasmBrowser& b = browser();
    if (!b.alive) return 0;
    return b.shell->handle_click(x, y) ? 1 : 0;
}

int nuby_wheel(int dy) {
    WasmBrowser& b = browser();
    if (!b.alive) return 0;
    return b.shell->handle_wheel(dy) ? 1 : 0;
}

int nuby_char(unsigned int codepoint) {
    WasmBrowser& b = browser();
    if (!b.alive || codepoint == 0) return 0;
    return b.shell->handle_char((uint32_t)codepoint) ? 1 : 0;
}

int nuby_key(const char* key) {
    WasmBrowser& b = browser();
    if (!b.alive || !key) return 0;
    return b.shell->handle_key(key) ? 1 : 0;
}

void nuby_resize(int w, int h) {
    WasmBrowser& b = browser();
    if (b.alive && w > 0 && h > 0) b.shell->set_viewport(w, h);
}

// Nav. directa (barra de direcciones → misma vía que teclear y pulsar Enter)
int nuby_goto(const char* url) {
    WasmBrowser& b = browser();
    if (!b.alive || !url) return 0;
    b.shell->go(url);
    return 1;
}

// Parpadeo del cursor: si cambió algo, devuelve 1 (el anfitrión repinta).
int nuby_tick() {
    WasmBrowser& b = browser();
    if (!b.alive) return 0;
    bool changed = b.shell->tick_blink();
    if (b.shell->dirty()) { b.shell->clear_dirty(); changed = true; }
    return changed ? 1 : 0;
}

// ---------------------------------------------------------------------------
//  Frame: el mismo RLE de Nuby que manda el servidor nativo
// ---------------------------------------------------------------------------

const unsigned char* nuby_frame_rle(int* out_len) {
    WasmBrowser& b = browser();
    if (out_len) *out_len = 0;
    if (!b.alive) return nullptr;
    const auto& fb = b.shell->frame();
    if (fb.empty()) return nullptr;
    b.rle = nuby::server::encode_rle(fb, b.shell->width(), b.shell->height());
    if (out_len) *out_len = (int)b.rle.size();
    return b.rle.data();
}

// ---------------------------------------------------------------------------
//  Estado y búsqueda (JSON listo para la página anfitriona)
// ---------------------------------------------------------------------------

// {"url":..,"status":..,"kbd":true|false,"text":..,"docs":N,"terms":N}
const char* nuby_state() {
    WasmBrowser& b = browser();
    if (!b.alive) { b.text = "{}"; return b.text.c_str(); }
    std::string u = b.shell->current_url();
    std::string st = b.shell->status();
    b.text = "{\"url\":\"" + json_escape(u) + "\",\"status\":\"" + json_escape(st) +
             "\",\"kbd\":" + (b.shell->text_focused() ? "true" : "false") +
             ",\"text\":\"" + json_escape(b.shell->focused_text()) +
             "\",\"docs\":" + std::to_string(nuby_documents()) +
             ",\"terms\":" + std::to_string(nuby_terms()) + "}";
    return b.text.c_str();
}

// Búsqueda BM25 real, en el mismo formato que la API /api/search del servidor.
const char* nuby_search(const char* q) {
    WasmBrowser& b = browser();
    if (!b.alive || !q) { b.text = "{\"results\":[]}"; return b.text.c_str(); }
    auto hits = b.shell->search(q);
    std::string j = "{\"query\":\"" + json_escape(q) + "\",\"results\":[";
    for (size_t i = 0; i < hits.size(); ++i) {
        j += (i ? "," : "");
        j += "{\"title\":\"" + json_escape(hits[i].doc->title) + "\",\"url\":\"" +
             json_escape(hits[i].doc->url) + "\",\"domain\":\"" +
             json_escape(hits[i].doc->domain) + "\",\"score\":" +
             std::to_string((int)hits[i].score) + ",\"snippet\":\"" +
             json_escape(hits[i].snippet) + "\"}";
    }
    j += "]}";
    b.text = j;
    return b.text.c_str();
}

// Historial real de la sesión (lo mismo que muestra la vista Historial)
const char* nuby_history() {
    WasmBrowser& b = browser();
    if (!b.alive) { b.text = "[]"; return b.text.c_str(); }
    std::string j = "[";
    bool first = true;
    for (const auto& h : b.shell->history()) {
        if (!first) j += ",";
        first = false;
        j += "{\"url\":\"" + json_escape(h.url) + "\",\"title\":\"" +
             json_escape(h.title) + "\",\"when\":\"" + json_escape(h.when) + "\"}";
    }
    j += "]";
    b.text = j;
    return b.text.c_str();
}

} // extern "C"
