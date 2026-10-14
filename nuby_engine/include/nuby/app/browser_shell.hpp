#pragma once

// ============================================================================
// NUBY BROWSER SHELL — El navegador completo, pintado por el propio motor.
//
// Filosofía corregida (petición explícita del usuario):
//   ANTES: la interfaz de Nuby era HTML servido a Chrome. Chrome la pintaba.
//   AHORA: la interfaz (botones, barra de URL, páginas home/resultados,
//          errores) se define en HTML+CSS, pero la parsea, maqueta y
//          RASTERIZA el motor Nuby. Lo que viaja al cliente son PÍXELES que
//          Nuby calculó: el navegador del usuario actúa solo de monitor
//          (como un VNC/escritorio remoto). Ningún píxel de la UI lo dibuja
//          el navegador del usuario.
//
// Todo el comportamiento es real:
//   • ◀ ▶ ⟳ usan pilas de historial reales en memoria
//   • Click → hit-testing sobre el árbol de layout del motor, subiendo por
//     el DOM hasta el <a href> o data-action correspondiente
//   • La barra de URL es editable de verdad (caret, borrado, Enter navega)
//   • Las webs se descargan por red real (Fetcher) y se indexan al visitarlas
//   • La búsqueda usa el índice BM25 local (nada hardcodeado ni sintetizado)
//   • Los scripts inline de las páginas se ejecutan con el intérprete real
//   • Errores de red se muestran tal cual ocurren (sin disfraces)
// ============================================================================

#include "../nuby_engine.hpp"
#include "../net/fetcher.hpp"
#include "../search/search_index.hpp"
#if defined(__wasi__)
#include "../js/js_stub.hpp"      // build de navegador (sin excepciones C++)
#else
#include "../js/js_interp.hpp"
#endif
#include "../media/png_decoder.hpp"
#include "html_preprocess.hpp"
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include "../core/platform.hpp"
#include <sstream>

namespace nuby::app {

struct HistoryEntry {
    std::string url;
    std::string title;
    std::string when;
};

// Una descarga REAL del motor: cada página web que la sesión baja de la red
// queda registrada aquí (URL, título, bytes exactos del cuerpo, cuándo).
// Descargas = la verdad de lo que el motor ha traído de internet; no hay
// entradas inventadas ni de muestra.
struct DownloadEntry {
    std::string url;
    std::string title;
    size_t bytes{0};
    std::string when;
};

class BrowserShell {
public:
    // Ventana virtual de ESTA sesión (2026-08-09): cada visitante recibe
    // frames del tamaño real de SU pantalla (el monitor la reporta por
    // /event?k=resize). Antes era fijo 1024×640 y en un teléfono el frame
    // se encogía entero → texto ilegible. Ya no es constexpr: es por sesión.
    int W{1024};                        // ancho ventana virtual
    int H{640};                         // alto ventana virtual
    static constexpr int CHROME_H = 58; // UNA fila (omnibox), como manda el diseño moderno
    int width() const { return W; }
    int height() const { return H; }
    // Sin barra superior EN LA HOME (petición del usuario 2026-08-10: la
    // referencia no lleva chrome en el inicio). El resto de vistas la tienen.
    int chrome_h() const { return mode_ == Mode::HOME ? 0 : CHROME_H; }
    int visible_h() const { return H - chrome_h(); }

    // ---- Densidad de render (2026-10-14) ----
    // El layout (W, H, todo el CSS y las decisiones responsive) sigue en px
    // lógicos; el rasterizador pinta a px de dispositivo. Un móvil con
    // densidad 3 pide scale=3 y el frame sale 1170 px de ancho para una
    // ventana de 390: texto y bordes nítidos de verdad, no estirados.
    float render_scale_{1.0f};
    int pW() const { return static_cast<int>(std::lround(W * render_scale_)); }
    int pH() const { return static_cast<int>(std::lround(H * render_scale_)); }
    int pChromeH() const { return static_cast<int>(std::lround(chrome_h() * render_scale_)); }
    float render_scale() const { return render_scale_; }

    // Densidad efectiva: la pedida, recortada por el presupuesto de memoria.
    float effective_scale(float requested) const {
        if (requested <= 0.01f) requested = 1.0f;
        if (requested > 4.0f) requested = 4.0f;    // ningún móvil real pasa de 4
        // Tope por memoria del lienzo visible: en un monitor enorme la
        // densidad se recorta sola en vez de agotar la memoria del equipo.
        const double wanted = (double)W * (double)H * requested * requested;
        if (wanted <= FRAME_PIXEL_BUDGET) return requested;
        const float capped = (float)std::sqrt(FRAME_PIXEL_BUDGET / ((double)W * (double)H));
        return capped < 1.0f ? 1.0f : capped;
    }

    // Cambia la densidad y RE-RENDERIZA todo (los búferes cambian de tamaño).
    void set_render_scale(float requested) {
        const float sc = effective_scale(requested);
        if (std::abs(sc - render_scale_) < 0.001f) return;
        render_scale_ = sc;
        apply_scale();
        render_current_mode();
        rebuild_chrome();
    }

    // ---- Estado visible del documento (para el estado que se muestra y las
    // pruebas reales: alto del documento, posición del scroll y salto) ----
    int content_height() const { return content_height_; }
    int scroll_position() const { return scroll_y_; }
    int raster_top() const { return content_top_; }
    int raster_rows() const { return content_rows_; }
    // Rueda del ratón / dedo en el lienzo: baja el documento de verdad.
    bool scroll_by(int dy) { return handle_wheel(dy); }

    // Punto de entrada para las pruebas de pintado: carga un documento real por
    // el MISMO camino que una página descargada (parseo → CSS → layout →
    // rasterizado). Solo para pruebas; el navegador usa la red.
    void load_html_for_test(const std::string& html) {
        core::LockGuard<core::Mutex> lock(mutex_);
        mode_ = Mode::WEB;
        scroll_y_ = 0;
        content_top_ = 0;
        current_url_ = "about:test";
        current_title_ = "Documento de prueba";
        render_content_doc(html, "", {});
        rebuild_chrome();
    }

    // CSS propio de las vistas INTERNAS del navegador (inicio, resultados,
    // menú, historial, ajustes, descargas, acerca de). El margen de 8 px del
    // UA es correcto en las páginas web, pero en la interfaz del navegador
    // dejaba un marco blanco alrededor y la barra no llegaba al borde.
    static constexpr const char* UI_CSS = "body { margin: 0; }";

    // ---- Lupa dibujada con primitivas ----
    // El tipo de letra del motor NO trae U+2315 (⌕): el símbolo salía en
    // blanco y dejaba un hueco raro en la caja de búsqueda. Se dibuja con
    // primitivas del propio rasterizador: aro redondo (border-radius) + mango.
    static std::string lupa_html(int size_px, const char* color) {
        const int r = size_px / 2;
        const int hueco = size_px - 4;          // aro de 2 px
        const int rh = hueco / 2;
        std::ostringstream o;
        o << "<div style=\"display: flex; flex-direction: row; align-items: flex-end;\">"
             // aro: círculo relleno con otro círculo del color del fondo encima
             "<div style=\"width: " << size_px << "px; height: " << size_px
          << "px; background-color: " << color << "; border-radius: " << r << "px;\">"
             "<div style=\"width: " << hueco << "px; height: " << hueco
          << "px; background-color: #ffffff; border-radius: " << rh << "px; margin: 2px;\"></div></div>"
             // mango
             "<div style=\"width: " << std::max(3, size_px / 3) << "px; height: 3px; background-color: "
          << color << "; border-radius: 2px; margin-left: -2px;\"></div>"
             "</div>";
        return o.str();
    }

    void apply_scale() {
        engine_chrome_->set_render_scale(render_scale_);
        engine_content_->set_render_scale(render_scale_);
    }

    // Re-renderiza TODO a un nuevo tamaño de ventana (móvil ↔ escritorio).
    void set_viewport(int w, int h) {
        // caps honestos: ni micro-frames ni monstruos que revienten la RAM
        w = std::max(240, std::min(1920, w));
        h = std::max(320, std::min(2160, h));
        if (w == W && h == H) return;
        W = w; H = h;
        render_scale_ = effective_scale(render_scale_); // el tope depende del ancho
        engine_chrome_->set_render_scale(render_scale_);
        engine_content_->set_render_scale(render_scale_);
        engine_chrome_->set_viewport(W, CHROME_H);
        engine_content_->set_viewport(W, CONTENT_CAP);
        scroll_y_ = 0; // el alto visible cambió; el scroll viejo no aplica
        // Las páginas INTERNAS hornean medidas del viewport en su markup
        // (centrado por padding, caja responsiva): con un resize hay que
        // RE-GENERARLAS, no solo re-maquetar el HTML viejo (bug real: la
        // home quedaba centrada para el ancho anterior).
        render_current_mode();
        rebuild_chrome(); // incluye compose() del frame final
    }

    // Re-genera la página interna actual con el estado vivo (lo usan el
    // resize y las opciones de Configuración, que cambian la geometría).
    void render_current_mode() {
        switch (mode_) {
            case Mode::HOME: render_home(); break;
            case Mode::RESULTS:
                if (!search_of_.empty()) show_search_results(search_of_);
                else render_home();
                break;
            case Mode::ABOUT: show_about(about_back_); break;
            case Mode::HISTORY: show_history(); break;
            case Mode::MENU: show_menu(); break;
            case Mode::DOWNLOADS: show_downloads(); break;
            case Mode::SETTINGS: show_settings(); break;
            default: refresh_content(); break; // páginas web reales: mismo DOM, nuevo ancho
        }
    }
    static constexpr int CONTENT_CAP = 8000; // tope de alto de página (seguridad)
    // Presupuestos de memoria (px de dispositivo).
    //  - FRAME: el lienzo que se ve (chrome + pantalla) y se envía cada frame.
    //  - CONTENIDO: la franja pintada de la página. Ya NO se pinta la página
    //    entera: se pinta una VENTANA alrededor de lo visible (como hacen los
    //    navegadores reales), así el búfer no crece con lo largo del
    //    documento. Antes, una Wikipedia a densidad 3 pedía más de 100 MB.
    static constexpr double FRAME_PIXEL_BUDGET = 9.0e6;
    static constexpr double CONTENT_PIXEL_BUDGET = 12.0e6;
    // Ventana de contenido: media pantalla por encima y una y media por debajo
    // (2 pantallas y media en total). Al salir de ella se re-pinta solo esa
    // franja, sin re-hacer el layout.
    int window_back() const { return visible_h() / 2; }
    int window_rows() const { return std::max(1, visible_h() * 5 / 2); }

    // Multi-sesión REAL: varias sesiones comparten UN índice de búsqueda
    // (como un crawler colectivo), pero cada una tiene su propio navegador:
    // historial, página actual, cookies de foco — todo aislado por sesión.
    explicit BrowserShell(std::shared_ptr<search::SearchIndex> shared_index = nullptr)
        : index_sp_(std::move(shared_index)) {
        if (!index_sp_) index_sp_ = std::make_shared<search::SearchIndex>();
        engine_chrome_ = std::make_unique<NubyBrowserEngine>(W, CHROME_H);
        engine_content_ = std::make_unique<NubyBrowserEngine>(W, CONTENT_CAP);
        engine_content_->set_trim_to_document(true);
        engine_content_->set_content_pixel_budget((size_t)CONTENT_PIXEL_BUDGET);
        apply_scale();
        go_home();
    }

    void set_data_path(const std::string& pages_tsv) { pages_path_ = pages_tsv; }
    search::SearchIndex& index() { return *index_sp_; }

    // ---------------- Entrada de usuario ----------------

    bool handle_click(int x, int y) {
        core::LockGuard<core::Mutex> lock(mutex_);
        const int ch = chrome_h();
        if (y < ch) {
            return click_chrome(x, y);
        }
        return click_content(x, y - ch + scroll_y_);
    }

    bool handle_wheel(int dy) {
        core::LockGuard<core::Mutex> lock(mutex_);
        int max_scroll = std::max(0, content_height_ - visible_h());
        int old = scroll_y_;
        scroll_y_ = std::max(0, std::min(max_scroll, scroll_y_ + dy));
        if (scroll_y_ != old) {
            ensure_content_window();   // si se sale de la franja pintada, se re-pinta
            compose();
            return true;
        }
        return false;
    }

    bool handle_char(uint32_t codepoint) {
        core::LockGuard<core::Mutex> lock(mutex_);
        if (focus_ == Focus::NONE) return false;
        // Campo de formulario REAL de la página web: el texto vive en el
        // atributo `value` del propio elemento del DOM (como en los navegadores)
        if (focus_ == Focus::WEBFIELD) {
            auto el = web_field_.lock();
            if (!el) { focus_ = Focus::NONE; return false; }
            el->set_attribute("value", el->get_attribute("value") + utf8_encode(codepoint));
            refresh_content();
            return true;
        }
        std::string& field = (focus_ == Focus::URL) ? input_url_ : input_search_;
        if (select_all_) { field.clear(); select_all_ = false; } // comportamiento real de url-bar
        field += utf8_encode(codepoint);
        if (focus_ == Focus::SEARCH && mode_ == Mode::HOME) render_home();
        else if (focus_ == Focus::SEARCH && mode_ == Mode::RESULTS) {
            // Instant Search real (BM25 en μs por tecla) — o, si el usuario
            // la apagó en Configuración, solo se re-pinta la caja (caché).
            if (setting_instant_) show_search_results(input_search_);
            else render_results_display();
        }
        rebuild_chrome();
        return true;
    }

    bool handle_key(const std::string& key) {
        core::LockGuard<core::Mutex> lock(mutex_);
        if (key == "Backspace") {
            if (focus_ == Focus::NONE) return false;
            if (focus_ == Focus::WEBFIELD) {
                auto el = web_field_.lock();
                if (!el) { focus_ = Focus::NONE; return false; }
                std::string v = el->get_attribute("value");
                if (v.empty()) return false;
                pop_utf8(v);
                el->set_attribute("value", v);
                refresh_content();
                return true;
            }
            std::string& field = (focus_ == Focus::URL) ? input_url_ : input_search_;
            if (select_all_) { field.clear(); select_all_ = false; }
            if (field.empty()) return false;
            pop_utf8(field);
            if (focus_ == Focus::SEARCH && mode_ == Mode::HOME) render_home();
            else if (focus_ == Focus::SEARCH && mode_ == Mode::RESULTS) {
                if (setting_instant_) show_search_results(input_search_);
                else render_results_display();
            }
            rebuild_chrome();
            return true;
        }
        if (key == "Enter") {
            if (focus_ == Focus::WEBFIELD) {
                auto el = web_field_.lock();
                if (!el) { focus_ = Focus::NONE; return false; }
                if (el->get_tag_name() == "textarea") {
                    // textarea real: Enter es salto de línea, no envío
                    el->set_attribute("value", el->get_attribute("value") + "\n");
                    refresh_content();
                    return true;
                }
                // Enter dentro de un <form>: envío IMPLÍCITO real (submit)
                auto clicked = el;
                focus_ = Focus::NONE;
                unfocus_webfield();
                submit_form(clicked);
                return true;
            }
            if (focus_ == Focus::URL) {
                focus_ = Focus::NONE;
                std::string target = normalize_user_input(input_url_);
                rebuild_chrome();
                navigate(target);
                return true;
            }
            if (focus_ == Focus::SEARCH) {
                focus_ = Focus::NONE;
                std::string q = input_search_;
                rebuild_chrome();
                // Google no dispara búsquedas vacías: no-op honesto
                if (!q.empty()) show_search_results(q);
                return true;
            }
            return false;
        }
        if (key == "Escape") {
            if (focus_ == Focus::NONE) return false;
            if (focus_ == Focus::WEBFIELD) {
                focus_ = Focus::NONE;
                unfocus_webfield();
                refresh_content();
                return true;
            }
            focus_ = Focus::NONE;
            input_url_ = current_url_;
            input_search_.clear();
            rebuild_chrome();
            if (mode_ == Mode::HOME) render_home();
            return true;
        }
        return false;
    }

    // ---------------- Salida de píxeles ----------------

    // Framebuffer completo W×H en RGBA (producido 100% por el motor)
    const std::vector<uint32_t>& frame() { return frame_; }

    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Re-render por parpadeo del caret cuando hay foco (lo llama el server
    // solo si pasaron ~500ms; devuelve true si cambió el frame)
    bool tick_blink() {
        core::LockGuard<core::Mutex> lock(mutex_);
        if (focus_ == Focus::NONE) return false;
        if (!setting_blink_) {
            // opción REAL: parpadeo desactivado → cursor FIJO, cero re-renders
            if (!caret_on_) { caret_on_ = true; rebuild_chrome(); return true; }
            return false;
        }
        bool on = (now_ms() / 550) % 2 == 0;
        if (on == caret_on_) return false;
        caret_on_ = on;
        rebuild_chrome();
        return true;
    }

    // Repinta la home tras cargar el índice (la primera salió a 0 docs)
    void reload_home() {
        core::LockGuard<core::Mutex> lock(mutex_);
        if (mode_ == Mode::HOME) { go_home(); }
    }

    // ---------------- Búsqueda pública (para la API) ----------------
    std::vector<search::SearchHit> search(const std::string& q) {
        return index_sp_->query(q, 24);
    }
    const std::deque<HistoryEntry>& history() const { return history_; }
    const std::deque<DownloadEntry>& downloads() const { return downloads_; }

    // ---- Puente de teclado del monitor (móvil/PC) -------------------------
    // El monitor (canvas remoto) necesita saber si AHORA hay un campo de
    // texto activo para abrir/cerrar el teclado virtual del teléfono.
    bool text_focused() const {
        return focus_ == Focus::URL || focus_ == Focus::SEARCH || focus_ == Focus::WEBFIELD;
    }
    // El contenido REAL del campo enfocado (para sincronizar el captador)
    std::string focused_text() const {
        switch (focus_) {
            case Focus::URL: return input_url_;
            case Focus::SEARCH: return input_search_;
            case Focus::WEBFIELD: {
                auto el = web_field_.lock();
                return el ? el->get_attribute("value") : std::string();
            }
            default: return {};
        }
    }

    // Registro REAL de descarga (lo llama load_web tras bajar una página).
    // Público para que las pruebas verifiquen el comportamiento sin red.
    void record_download(const std::string& url, const std::string& title, size_t bytes) {
        if (url.rfind("nuby://", 0) == 0) return; // páginas internas no son descargas
        downloads_.push_front(DownloadEntry{url, title, bytes, timestamp_now()});
        while (downloads_.size() > 60) downloads_.pop_back();
    }

    // Navegación pública (para /api/goto): misma vía real que teclear la URL
    void go(const std::string& url) {
        core::LockGuard<core::Mutex> lock(mutex_);
        navigate(url);
        dirty_ = true;
    }
    std::string current_url() { core::LockGuard<core::Mutex> lock(mutex_); return current_url_; }
    std::string status() { core::LockGuard<core::Mutex> lock(mutex_); return status_; }

private:
    enum class Focus { NONE, URL, SEARCH, WEBFIELD }; // WEBFIELD: <input>/<textarea> REALES de la página
    enum class Mode { HOME, WEB, RESULTS, ERROR_PAGE, ABOUT, HISTORY, MENU, DOWNLOADS, SETTINGS };

    // ---------------- Estado ----------------
    core::Mutex mutex_;
    std::unique_ptr<NubyBrowserEngine> engine_chrome_;
    std::unique_ptr<NubyBrowserEngine> engine_content_;

    std::string current_url_{"nuby://home"};
    std::string current_title_{"Nuby"};
    std::string status_{"Listo"};
    Mode mode_{Mode::HOME};
    Focus focus_{Focus::NONE};

    std::string input_url_{"nuby://home"};
    std::string input_search_;
    std::string search_of_;
    std::string about_back_{"nuby://home"};
    bool select_all_{false};

    int scroll_y_{0};
    // Cobertura REAL del búfer de contenido (px lógicos): de content_top_ a
    // content_top_ + content_rows_. Fuera de esa franja hay que re-pintar.
    int content_top_{0};
    int content_rows_{0};
    int content_height_{H - CHROME_H};
    bool caret_on_{true};

    std::vector<uint32_t> frame_;       // W*H compuesto
    std::vector<uint32_t> content_fb_;  // búfer de contenido en px de DISPOSITIVO (recortado al documento)
    bool dirty_{true};

    std::vector<std::string> back_stack_;
    std::vector<std::string> fwd_stack_;
    std::deque<HistoryEntry> history_;
    std::deque<DownloadEntry> downloads_; // páginas REALES bajadas por el motor

    // ---- Configuración REAL (cada opción cambia comportamiento de verdad) --
    bool setting_blink_{true};    // cursor parpadeante (tick_blink lo consulta)
    bool setting_history_{true};  // guardar historial (record_visit lo consulta)
    bool setting_instant_{true};  // búsqueda instantánea al teclear

    // Caché REAL de la última búsqueda: con la instantánea apagada, la caja
    // se re-pinta sin volver a consultar el índice hasta el próximo Enter.
    std::vector<search::SearchHit> last_hits_;
    double last_query_secs_{0.0};

    std::shared_ptr<search::SearchIndex> index_sp_;
    std::string pages_path_;

    std::shared_ptr<RenderResult> content_result_; // para hit-testing
    std::shared_ptr<js::Interpreter> js_;          // intérprete de la página actual
    std::string page_css_;                          // CSS de la página web actual (para re-renders)
    std::weak_ptr<html::Element> web_field_;        // campo de formulario enfocado (si hay)
    html::Element* caret_el_obs_ = nullptr;         // elemento que lleva el marcador de caret (observado, no poseído)
    std::vector<std::string> page_scripts_;        // scripts inline de la página

    // ---------------- Utilidades ----------------
    static long now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    static std::string utf8_encode(uint32_t cp) {
        std::string out;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        return out;
    }
    static void pop_utf8(std::string& s) {
        while (!s.empty()) {
            char c = s.back();
            s.pop_back();
            if ((c & 0xC0) != 0x80) break; // borra hasta el líder UTF-8
        }
    }
    static std::string esc(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '&') r += "&amp;";
            else if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '"') r += "&quot;";
            else r += c;
        }
        return r;
    }

    std::string normalize_user_input(const std::string& in) {
        std::string s = core::StringUtils::trim(in);
        if (s.empty()) return "nuby://home";
        if (s.find("://") == std::string::npos) {
            // ¿parece dominio o es una búsqueda?
            if (s.find(' ') == std::string::npos && s.find('.') != std::string::npos)
                return "https://" + s;
            return "nuby://search?q=" + url_encode(s);
        }
        return s;
    }

    static std::string url_encode(const std::string& s) {
        std::string r;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
            else {
                char buf[4];
                snprintf(buf, sizeof buf, "%%%02X", c);
                r += buf;
            }
        }
        return r;
    }
    static std::string url_decode(const std::string& s) {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int hi = core::StringUtils::to_int(std::string(1, s[i + 1]), -1);
                if (hi >= 0 && hi <= 9) hi = hi;
                else if (s[i+1] >= 'a' && s[i+1] <= 'f') hi = s[i+1] - 'a' + 10;
                else if (s[i+1] >= 'A' && s[i+1] <= 'F') hi = s[i+1] - 'A' + 10;
                else hi = -1;
                int lo = -1;
                if (s[i+2] >= '0' && s[i+2] <= '9') lo = s[i+2] - '0';
                else if (s[i+2] >= 'a' && s[i+2] <= 'f') lo = s[i+2] - 'a' + 10;
                else if (s[i+2] >= 'A' && s[i+2] <= 'F') lo = s[i+2] - 'A' + 10;
                if (hi >= 0 && lo >= 0) {
                    r += (char)((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            if (s[i] == '+') { r += ' '; continue; }
            r += s[i];
        }
        return r;
    }

    // ---------------- Chrome (barra superior) HTML ----------------
    // Profesional = limpio: UNA fila con navegación + omnibox. Nada de
    // letreros técnicos permanentes (ningún navegador real los tiene).
    std::string chrome_html() const {
        bool url_focused = (focus_ == Focus::URL);
        std::string shown = url_focused ? input_url_ : current_url_;
        // Se ven los últimos caracteres, con el ancho REAL que deja la barra:
        // en un móvil quedan ~160 px para la caja, así que el texto se recorta
        // DENTRO de la caja (antes se salía del marco y se veía cortado).
        const int ancho_botones = 175 + (W >= 640 ? 60 : 0);
        size_t max_vis = (size_t)std::max(10, (W - ancho_botones) / 8);
        if (shown.size() > max_vis) shown = "…" + shown.substr(shown.size() - max_vis);

        std::string caret;
        if (url_focused && caret_on_)
            caret = "<div style=\"width: 2px; height: 20px; background-color: #1a73e8; margin-left: 1px;\"></div>";

        std::string lock;
        if (W >= 520) {   // en móvil ese hueco se lo queda la propia dirección
        if (current_url_.rfind("https://", 0) == 0)
            lock = "<span style=\"color: #188038; font-size: 12px; font-weight: 700;\">TLS </span>";
        else if (current_url_.rfind("http://", 0) == 0)
            lock = "<span style=\"color: #b45309; font-size: 12px; font-weight: 700;\">http </span>";
        }

        std::string ring = url_focused ? "#1a73e8" : "#dadce0";

        auto navbtn = [](const char* action, const char* sym) {
            return std::string("<div data-action=\"") + action +
                   "\" style=\"background-color: #f1f3f4; padding: 5px 12px; border-radius: 8px;\">"
                   "<span style=\"color: #3c4043; font-size: 15px; font-weight: 700;\">" + sym +
                   "</span></div>";
        };

        std::ostringstream h;
        h << "<div style=\"background-color: #ffffff; border-bottom: 1px solid #dadce0; padding: 8px 10px;\">"
             "<div style=\"display: flex; flex-direction: row; align-items: center; gap: 7px;\">";
        // ☰ Menú real (Historial/Descargas/Configuración/Acerca de): tres
        // barras = tres cajas 14×2 que RASTERIZA el motor, no un emoji.
        h << "<div data-action=\"menu\" style=\"background-color: #f1f3f4; padding: 7px 10px; border-radius: 8px;\">"
             "<div style=\"width: 14px; height: 2px; background-color: #3c4043; margin-bottom: 3px;\"></div>"
             "<div style=\"width: 14px; height: 2px; background-color: #3c4043; margin-bottom: 3px;\"></div>"
             "<div style=\"width: 14px; height: 2px; background-color: #3c4043;\"></div>"
             "</div>";
        if (W >= 640) // en móvil el espacio es oro: sin logo en el chrome
            h << "<div data-action=\"home\" style=\"margin-right: 4px;\">"
                 "<span style=\"color: #202124; font-size: 18px; font-weight: 800;\">Nuby</span></div>";
        h << navbtn("back", "&lt;") << navbtn("fwd", "&gt;") << navbtn("reload", "R")
          << "<div data-action=\"focus-url\" style=\"flex-grow: 1; background-color: #f1f3f4; border: 2px solid " << ring
          << "; border-radius: 18px; padding: 6px 12px;\">"
             "<div style=\"display: flex; flex-direction: row; align-items: center;\">" << lock
          << "<span style=\"color: #202124; font-size: 15px;\">" << esc(shown) << "</span>" << caret
          << "</div></div>"
             "</div>"
          "</div>";
        return h.str();
    }

    void rebuild_chrome() {
        auto res = std::make_shared<RenderResult>(
            engine_chrome_->render_page(chrome_html()));
        chrome_result_ = res;
        chrome_fb_ = res->pixels;
        compose();
    }

    // ---------------- Composición: chrome + contenido → frame ----------------
    void compose() {
        // Composición en píxeles de DISPOSITIVO: el chrome y el contenido ya
        // vienen rasterizados a densidad (W*escala de ancho). El scroll sigue
        // pensado en filas lógicas, y aquí se traduce a filas de dispositivo.
        const int pw = pW();
        const int ph = pH();
        const int ch = pChromeH();
        if (frame_.size() != (size_t)pw * ph) frame_.assign((size_t)pw * ph, 0xFFFFFFFF);
        if (ch > 0) {
            if (chrome_fb_.size() != (size_t)pw * ch) rebuild_chrome_only();
            // 1) chrome (solo cuando la vista actual lo muestra; la home no)
            const size_t n = std::min(chrome_fb_.size(), frame_.size());
            std::copy(chrome_fb_.begin(), chrome_fb_.begin() + n, frame_.begin());
        }
        // 2) contenido (slice con scroll). scroll_y_ está en filas LÓGICAS y el
        // búfer en filas de DISPOSITIVO: una fila lógica son `sc` filas del
        // búfer. Se saltan las filas que no existen (nunca se lee de más).
        const float sc = render_scale_ > 0.01f ? render_scale_ : 1.0f;
        const size_t buf_rows = content_fb_.size() / (size_t)pw;   // filas reales del búfer
        const int content_rows = ph - ch;
        for (int drow = 0; drow < content_rows; ++drow) {
            const int logical_row = (int)((float)drow / sc);
            const int page_row = scroll_y_ + logical_row;       // fila del DOCUMENTO
            const int in_buf = page_row - content_top_;         // fila dentro del búfer
            const size_t src_row = (size_t)((float)in_buf * sc);
            auto dst = frame_.begin() + (size_t)(ch + drow) * pw;
            if (in_buf >= 0 && src_row < buf_rows) {
                std::copy(content_fb_.begin() + src_row * pw,
                          content_fb_.begin() + src_row * pw + pw, dst);
            } else {
                std::fill(dst, dst + pw, 0xFFFFFFFF);
            }
        }
        dirty_ = true;
    }

    // ---------------- Adopción del búfer de contenido ----------------
    // El motor ya recortó el búfer al alto REAL del documento (densidad
    // incluida). Aquí se adopta tal cual y el alto usable se deriva de las
    // filas que EXISTEN de verdad en el búfer: el scroll nunca puede pedir
    // una fila que no esté pintada (antes se leía de más o se quedaba en
    // blanco según el caso).
    void adopt_content(const std::shared_ptr<RenderResult>& res) {
        if (!res) return;
        content_fb_ = res->pixels;
        content_top_ = res->raster_top_logical;
        content_rows_ = res->raster_rows_logical;   // filas lógicas que cubre el búfer
        // Alto del DOCUMENTO (para el scroll): se mide del layout, no del búfer
        // (el búfer es solo una franja).
        int doc_rows = visible_h();
        if (res->layout_tree) {
            float max_bottom = 0;
            walk_bottom(res->layout_tree, max_bottom);
            doc_rows = (int)max_bottom + 12;
        }
        content_height_ = std::max(visible_h(), std::min(doc_rows, CONTENT_CAP));
    }

    // ---------------- Scroll por ventana ----------------
    // Si lo visible ya no cabe en la franja pintada, se re-pinta SOLO esa
    // franja (sin re-layout ni scripts: el DOM no se toca).
    void ensure_content_window() {
        if (!content_result_ || !content_result_->document) return;
        const int vis = visible_h();
        if (content_rows_ > 0 && scroll_y_ >= content_top_ &&
            scroll_y_ + vis <= content_top_ + content_rows_) {
            return;   // la franja actual ya cubre lo que se ve
        }
        const int top = std::max(0, std::min(scroll_y_ - window_back(),
                                             std::max(0, CONTENT_CAP - vis)));
        const int rows = std::min(window_rows(), std::max(vis, CONTENT_CAP - top));
        auto win = engine_content_->rasterize_window(top, rows);
        if (win.pixels.empty()) return;
        content_fb_ = std::move(win.pixels);
        content_top_ = win.top_logical;
        content_rows_ = win.rows_logical;
        if (content_result_) {
            // El búfer del RenderResult queda como el que de verdad se usa
            content_result_->pixels = content_fb_;
            content_result_->raster_top_logical = content_top_;
            content_result_->raster_rows_logical = content_rows_;
        }
    }
    void rebuild_chrome_only() {
        auto res = engine_chrome_->render_page(chrome_html());
        chrome_result_ = std::make_shared<RenderResult>(std::move(res));
        chrome_fb_ = chrome_result_->pixels;
    }

    // Pinta la página DESDE ARRIBA con una ventana de 2,5 pantallas: el búfer
    // no crece con lo largo del documento (al bajar se re-pinta la franja).
    RenderResult paint_page_top(const std::string& html, const std::string& css) {
        engine_content_->set_raster_window(0, window_rows());
        return engine_content_->render_page(html, css);
    }
    RenderResult paint_doc_top(const std::shared_ptr<html::Document>& doc, const std::string& css) {
        engine_content_->set_raster_window(0, window_rows());
        return engine_content_->render_document(doc, css);
    }

    // ---------------- Render del documento de contenido ----------------
    void render_content_doc(const std::string& html_clean, const std::string& css,
                            const std::vector<std::string>& scripts) {
        page_css_ = UA_EXTRA_CSS + "\n" + css;
        auto res = std::make_shared<RenderResult>(
            paint_page_top(html_clean, page_css_));
        content_result_ = res;
        adopt_content(res);   // búfer + alto real rasterizado (documento, no 8000 filas)

        // Ejecuta los scripts inline con el intérprete REAL
        js_ = std::make_shared<js::Interpreter>(res->document);
        bool mutated = false;
        for (auto& code : scripts) {
#if defined(__wasi__)
            js_->run(code);   // el stub de la build de navegador no lanza
#else
            try {
                js_->run(code);
            } catch (const std::exception& e) {
                js_->clear_mutation();
                status_ = std::string("JS detenido: ") + e.what();
                break;
            }
#endif
            if (js_->dom_mutated()) { mutated = true; js_->clear_mutation(); }
        }
        if (mutated) {
            // El DOM cambió de verdad → repinta con el DOM mutado
            res = std::make_shared<RenderResult>(
                paint_page_top(serialize_body(res->document), UA_EXTRA_CSS + "\n" + css));
            content_result_ = res;
            adopt_content(res);
            // Re-bind del intérprete al documento nuevo (el anterior quedó muerto)
            {
                auto eng = engine_content_->get_js_engine();
                js_ = eng ? eng->interpreter() : nullptr;
            }
        }
        scroll_y_ = 0;
        compose();
    }

    static void walk_bottom(const std::shared_ptr<layout::LayoutBox>& box, float& max_bottom) {
        if (!box) return;
        max_bottom = std::max(max_bottom, box->dimensions.border_box().bottom());
        for (auto& c : box->children) walk_bottom(c, max_bottom);
    }

    // Serializa el body (tras mutaciones JS) para re-render honesto
    static std::string serialize_body(const std::shared_ptr<html::Document>& doc) {
        if (!doc || !doc->get_body()) return "";
        std::string out;
        for (auto& child : doc->get_body()->get_children()) out += serialize_node(child);
        return out;
    }
    static std::string serialize_node(const std::shared_ptr<html::Node>& node) {
        if (node->is_text()) return esc(std::static_pointer_cast<html::TextNode>(node)->get_text());
        if (!node->is_element()) return "";
        auto el = std::static_pointer_cast<html::Element>(node);
        std::string tag = el->get_tag_name();
        if (tag == "script" || tag == "style") return "";
        std::string out = "<" + tag;
        for (auto& [k, v] : el->get_attributes()) out += " " + k + "=\"" + esc(v) + "\"";
        out += ">";
        for (auto& child : el->get_children()) out += serialize_node(child);
        out += "</" + tag + ">";
        return out;
    }

    // ---------------- Hit-testing ----------------

    // Busca la caja de layout más profunda que contiene (x, y)
    static std::shared_ptr<layout::LayoutBox> deepest_at(
            const std::shared_ptr<layout::LayoutBox>& box, float x, float y) {
        if (!box) return nullptr;
        auto bb = box->dimensions.border_box();
        bool inside = x >= bb.x && x < bb.right() && y >= bb.y && y < bb.bottom();
        // Los TEXT_BOX miden su rect por text_runs; pruébalos también
        if (!inside && !box->text_runs.empty()) {
            for (auto& r : box->text_runs) {
                if (x >= r.rect.x && x < r.rect.right() && y >= r.rect.y && y < r.rect.bottom()) {
                    inside = true; break;
                }
            }
        }
        if (!inside) return nullptr;
        for (auto& c : box->children) {
            if (auto hit = deepest_at(c, x, y)) return hit;
        }
        return box;
    }

    // Sube por el DOM buscando un ancestro “accionable”
    static std::shared_ptr<html::Element> actionable_ancestor(std::shared_ptr<html::Node> node) {
        while (node) {
            if (node->is_element()) {
                auto el = std::static_pointer_cast<html::Element>(node);
                const std::string& tag = el->get_tag_name();
                if (el->has_attribute("data-action") || el->has_attribute("data-nuby-input") ||
                    (tag == "a" && el->has_attribute("href")) ||
                    el->has_attribute("onclick") ||
                    tag == "input" || tag == "textarea" || tag == "button" || tag == "select")
                    return el;
            }
            node = node->get_parent();
        }
        return nullptr;
    }

    bool click_chrome(int x, int y) {
        if (!chrome_result_ || !chrome_result_->layout_tree) return false;
        auto box = deepest_at(chrome_result_->layout_tree, (float)x, (float)y);
        if (!box) return false;
        auto el = actionable_ancestor(box->node);
        if (!el) return false;
        std::string action = el->get_attribute("data-action");

        if (action == "focus-url") {
            focus_ = Focus::URL;
            input_url_ = current_url_;
            select_all_ = true; // como Chrome: todo seleccionado al enfocar
            caret_on_ = true;
            status_ = "Editando direccion — Enter para navegar, Esc para cancelar";
            rebuild_chrome();
            return true;
        }
        focus_ = Focus::NONE;
        if (action == "menu") { navigate("nuby://menu"); return true; }
        if (action == "back" && !back_stack_.empty()) {
            fwd_stack_.push_back(current_url_);
            std::string u = back_stack_.back(); back_stack_.pop_back();
            navigate(u, /*record=*/false);
        } else if (action == "fwd" && !fwd_stack_.empty()) {
            back_stack_.push_back(current_url_);
            std::string u = fwd_stack_.back(); fwd_stack_.pop_back();
            navigate(u, /*record=*/false);
        } else if (action == "reload") {
            navigate(current_url_, /*record=*/false);
        } else if (action == "home") {
            navigate("nuby://home");
        } else if (action == "about") {
            navigate("nuby://about");
        } else if (action == "clear-history") {
            history_.clear();
            show_history();
        }
        return true;
    }

    bool click_content(int x, int y_in_doc) {
        if (!content_result_ || !content_result_->layout_tree) return false;
        auto box = deepest_at(content_result_->layout_tree, (float)x, (float)y_in_doc);
        if (!box) { focus_ = Focus::NONE; rebuild_chrome(); return false; }
        auto el = actionable_ancestor(box->node);
        focus_ = Focus::NONE;
        unfocus_webfield();
        rebuild_chrome();
        if (!el) return false;

        // -------- Controles de formulario REALES de la página web --------
        {
            const std::string& tag = el->get_tag_name();
            if (tag == "input" || tag == "textarea" || tag == "button") {
                std::string type = core::StringUtils::to_lower(el->get_attribute("type"));
                if (type == "checkbox" || type == "radio") {
                    // guarda el default para <input type=reset> (primer toggle)
                    if (!el->has_attribute("data-nuby-default-checked"))
                        el->set_attribute("data-nuby-default-checked",
                                          el->has_attribute("checked") ? "1" : "0");
                    // toggle REAL: el atributo checked del DOM es la verdad
                    if (type == "radio") {
                        // radio real: desmarca los del mismo `name` en el formulario
                        std::string myname = el->get_attribute("name");
                        for (auto& cand : collect_elements(el, "input")) {
                            if (cand != el && cand->get_attribute("name") == myname &&
                                core::StringUtils::to_lower(cand->get_attribute("type")) == "radio")
                                cand->remove_attribute("checked");
                        }
                        el->set_attribute("checked", "");
                    } else if (el->has_attribute("checked")) {
                        el->remove_attribute("checked");
                    } else {
                        el->set_attribute("checked", "");
                    }
                    refresh_content();
                    return true;
                }
                if (type == "submit" || type == "button" || type == "reset" || tag == "button") {
                    if (type == "reset") { reset_form(el); refresh_content(); return true; }
                    submit_form(el);
                    return true;
                }
                if (type == "hidden" || type == "file" || type == "image") return false;
                // Campo de texto (text/search/email/url/password/number/tel/vacío):
                // foco REAL + caret visible. Guarda el valor inicial para reset.
                if (!el->has_attribute("data-nuby-default"))
                    el->set_attribute("data-nuby-default", el->get_attribute("value"));
                focus_ = Focus::WEBFIELD;
                web_field_ = el;
                el->set_attribute("data-nuby-caret", "1");
                caret_el_obs_ = el.get();
                caret_on_ = true;
                status_ = "Editando campo '" +
                          (el->get_attribute("name").empty() ? std::string("sin-nombre") : el->get_attribute("name")) +
                          "' — Enter envía el formulario, Esc sale";
                refresh_content();
                return true;
            }
            if (tag == "select") {
                // select honesto: aún no interactivo (documentado)
                status_ = "<select> no interactivo todavía — documentado";
                rebuild_chrome();
                return true;
            }
        }

        // -------- Navegación interna del menú desde el contenido --------
        const std::string act = el->get_attribute("data-action");
        if (act == "menu") { navigate("nuby://menu"); return true; }

        // Botón "Buscar con Nuby" (mismo camino que Enter: cero duplicación)
        if (act == "do-search") {
            focus_ = Focus::NONE;
            rebuild_chrome();
            if (!input_search_.empty()) show_search_results(input_search_);
            return true;
        }

        // Input propio de Nuby (caja de búsqueda en home y en resultados)
        if (el->has_attribute("data-nuby-input")) {
            focus_ = Focus::SEARCH;
            if (mode_ == Mode::RESULTS) {
                // como Google: la caja de resultados edita LA QUERY ACTUAL
                input_search_ = search_of_;
                show_search_results(search_of_);
            } else {
                input_search_.clear();
                render_home();
            }
            rebuild_chrome();
            return true;
        }

        // onclick: JavaScript real primero
        if (js_ && el->has_attribute("id")) {
#if defined(__wasi__)
            if (js_->dispatch_click(el->get_attribute("id"))) {
                status_ = "Script ejecutado";
                render_after_js();
                return true;
            }
#else
            try {
                if (js_->dispatch_click(el->get_attribute("id"))) {
                    status_ = "Script ejecutado";
                    render_after_js();
                    return true;
                }
            } catch (const std::exception& e) {
                status_ = std::string("JS detenido: ") + e.what();
                rebuild_chrome();
                return true;
            }
#endif
        }

        // Enlaces reales
        if (el->get_tag_name() == "a" && el->has_attribute("href")) {
            std::string href = el->get_attribute("href");
            if (href.rfind("javascript:", 0) == 0) {
                if (js_) {
#if defined(__wasi__)
                    js_->run(href.substr(11));
                    render_after_js();
#else
                    try { js_->run(href.substr(11)); render_after_js(); }
                    catch (const std::exception& e) { status_ = std::string("JS: ") + e.what(); rebuild_chrome(); }
#endif
                    return true;
                }
                return false;
            }
            std::string target = net::Fetcher::resolve_url(current_url_, href);
            navigate(target);
            return true;
        }
        return false;
    }

    void render_after_js() {
        if (js_ && js_->dom_mutated() && content_result_ && content_result_->document) {
            js_->clear_mutation();
            // Documento VIVO (no serializar+re-parsear): el intérprete y su
            // estado JS (funciones, variables, handlers) SOBREVIVEN al repintado.
            auto res = std::make_shared<RenderResult>(
                paint_doc_top(content_result_->document,
                              page_css_.empty() ? UA_EXTRA_CSS : page_css_));
            content_result_ = res;
            adopt_content(res);
            // re-bind (mismo documento → mismo intérprete conservado)
            {
                auto eng = engine_content_->get_js_engine();
                js_ = eng ? eng->interpreter() : nullptr;
                if (js_) js_->clear_mutation();
            }
        }
        compose();
    }

    // ---------------- Navegación ----------------
    void navigate(const std::string& url, bool record = true) {
        if (record && !current_url_.empty() && current_url_ != url) {
            back_stack_.push_back(current_url_);
            fwd_stack_.clear();
        }

        if (url == "nuby://home") { go_home(); return; }
        if (url.rfind("nuby://search", 0) == 0) {
            auto p = url.find("q=");
            show_search_results(p != std::string::npos ? url_decode(url.substr(p + 2)) : "");
            return;
        }
        if (url == "nuby://about") { show_about(record ? url : current_url_); if (record) {} return; }
        if (url == "nuby://history") { show_history(); return; }
        if (url == "nuby://clear-history") { history_.clear(); show_history(); return; }
        if (url == "nuby://menu") { show_menu(); return; }
        if (url == "nuby://downloads") { show_downloads(); return; }
        if (url == "nuby://clear-downloads") { downloads_.clear(); show_downloads(); return; }
        if (url == "nuby://settings") { show_settings(); return; }
        // nuby://set/<opcion>/<valor> — Configuración que SÍ aplica cambios
        if (url.rfind("nuby://set/", 0) == 0) { apply_setting(url.substr(11)); return; }

        load_web(url, "GET", "");
    }

    // Envío de formulario REAL por POST (application/x-www-form-urlencoded)
    void navigate_post(const std::string& url, const std::string& body) {
        if (!current_url_.empty() && current_url_ != url) {
            back_stack_.push_back(current_url_);
            fwd_stack_.clear();
        }
        load_web(url, "POST", body);
    }

    // Descarga y render REAL de una URL (compartido por GET y POST)
    void load_web(const std::string& url, const std::string& method, const std::string& req_body) {
        // --- Web real ---
        mode_ = Mode::WEB;
        current_url_ = url;
        input_url_ = url;
        status_ = method + " " + url + " …";
        scroll_y_ = 0;
        rebuild_chrome();

        auto t0 = now_ms();
        auto res = net::Fetcher::fetch(url, 5, method, req_body);
        long ms = now_ms() - t0;

        if (!res.error.empty()) {
            show_error(url, res.error);
            return;
        }
        if (res.status_code >= 400) {
            show_error(url, "HTTP " + std::to_string(res.status_code) + " " + res.status_message);
            return;
        }

        std::string ctype = core::StringUtils::to_lower(res.header("Content-Type"));
        bool looks_html = ctype.find("html") != std::string::npos || ctype.empty();

        if (!looks_html) {
            show_error(url, "Contenido no HTML (" + res.header("Content-Type") + ") — Nuby solo renderiza documentos");
            return;
        }

        current_url_ = res.final_url.empty() ? url : res.final_url;
        input_url_ = current_url_;

        // Charset honesto: latin1 → utf-8 si el servidor lo declara
        std::string body = res.body;
        if (ctype.find("iso-8859") != std::string::npos || ctype.find("latin1") != std::string::npos)
            body = preprocess::latin1_to_utf8(body);

        // Preproceso REAL: extrae <style>/<script>, convierte <img>/<svg> en placeholders
        preprocess::PreparedPage pp = preprocess::prepare(body, current_url_);

        // CSS externa (1 nivel, honesto)
        for (auto& css_href : pp.external_css) {
            auto css_res = net::Fetcher::fetch(css_href, 2);
            if (css_res.error.empty() && css_res.status_code < 400) {
                pp.inline_css += "\n" + preprocess::strip_at_rules(css_res.body);
            }
        }

        // Scripts inline REALES: van al intérprete antes del layout, así que
        // si el JS muta el DOM, lo mutado es lo que se pinta. Sin esto los
        // scripts recogidos por el preproceso jamás se ejecutaban.
        std::string js_all;
        for (const auto& sc : pp.inline_scripts) { js_all += sc; js_all += '\n'; }

        auto parse_res = std::make_shared<RenderResult>(
            engine_content_->render_page(pp.body_html, UA_EXTRA_CSS + "\n" + pp.inline_css, js_all));
        current_title_ = parse_res->document ? parse_res->document->get_title() : current_url_;
        if (current_title_.empty()) current_title_ = current_url_;

        status_ = "OK · " + std::to_string(res.body.size() / 1024) + " KB · red " +
                  std::to_string(ms) + " ms · HTTP " + std::to_string(res.status_code);

        // Reporte de JS honesto: si el intérprete reportó errores, se muestran.
        size_t js_err = 0;
        for (const auto& l : parse_res->js_logs)
            if (l.find("[Nuby JS error]") != std::string::npos) ++js_err;
        if (js_err) status_ += " · JS: " + std::to_string(js_err) + " error(es)";
        record_visit(current_url_, current_title_);
        // DESCARGA REAL registrada en el menú ☰ → Descargas: son los bytes
        // exactos del cuerpo HTTP que el motor acaba de traerse de la red.
        record_download(current_url_, current_title_, body.size());

        // Guarda la URL base resuelta para los enlaces relativos
        content_base_url_ = pp.base_href.empty() ? current_url_ : net::Fetcher::resolve_url(current_url_, pp.base_href);

        // Render + scripts (render_content_doc también mide altura real)
        content_result_ = parse_res;
        adopt_content(parse_res);

        // UN SOLO intérprete por página. ANTES los scripts corrían DOS veces
        // (una dentro de render_page y otra aquí) con estados separados: doble
        // ejecución y los onclick no veían las funciones definidas al cargar.
        // Ahora reutilizamos el intérprete y el ESTADO que render_page creó.
        {
            auto eng = engine_content_->get_js_engine();
            js_ = eng ? eng->interpreter() : nullptr;
            if (js_) js_->clear_mutation(); // esa mutación ya quedó pintada
        }
        // CSS vivo de ESTA página: los re-renders tras onclick deben conservarla
        // (antes se perdía y el contenido mutado salía sin estilos: texto oscuro
        // sobre fondo oscuro, casi invisible — bug real visto 2026-08-09)
        page_css_ = UA_EXTRA_CSS + "\n" + pp.inline_css;

        // ---- Carga de imágenes REALES (2026-08-09) -------------------------
        // Descarga cada <img>, huele los magic bytes y decodifica con el
        // decodificador PNG propio. JPEG/GIF → placeholder HONESTO con razón.
        // Las imágenes quedan adjuntas al documento VIVO y re-renderizamos.
        {
            static constexpr size_t MAX_IMGS = 12; // tope honesto por página
            auto imgs = parse_res->document
                ? parse_res->document->get_elements_by_tag_name("img")
                : std::vector<std::shared_ptr<html::Element>>{};
            size_t ok_imgs = 0, bad_imgs = 0, seen = 0;
            for (auto& im : imgs) {
                if (++seen > MAX_IMGS) break;
                std::string src = core::StringUtils::trim(im->get_attribute("src"));
                if (src.empty() || src.rfind("data:", 0) == 0) {
                    im->set_attribute("data-nuby-imgfail", src.empty() ? "src vacio" : "data: aun no soportado");
                    ++bad_imgs;
                    continue;
                }
                std::string abs = net::Fetcher::resolve_url(
                    content_base_url_.empty() ? current_url_ : content_base_url_, src);
                auto ires = net::Fetcher::fetch(abs, 3);
                if (!ires.error.empty() || ires.status_code >= 400) {
                    im->set_attribute("data-nuby-imgfail",
                        ires.error.empty() ? "HTTP " + std::to_string(ires.status_code) : ires.error);
                    ++bad_imgs;
                    continue;
                }
                auto& b = ires.body;
                if (b.size() >= 8 && (unsigned char)b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
                    auto img = std::make_shared<media::Image>();
                    std::string perr;
                    if (media::PngDecoder::decode(b, *img, perr)) {
                        im->decoded_image = img;
                        ++ok_imgs;
                    } else {
                        im->set_attribute("data-nuby-imgfail", "PNG: " + perr);
                        ++bad_imgs;
                    }
                } else if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) {
                    im->set_attribute("data-nuby-imgfail", "JPEG no soportado aun (decodificador propio pendiente)");
                    ++bad_imgs;
                } else if (b.size() >= 6 && b.rfind("GIF8", 0) == 0) {
                    im->set_attribute("data-nuby-imgfail", "GIF no soportado");
                    ++bad_imgs;
                } else {
                    im->set_attribute("data-nuby-imgfail", "formato no reconocido");
                    ++bad_imgs;
                }
            }
            if (ok_imgs + bad_imgs > 0) {
                status_ += " · imgs " + std::to_string(ok_imgs) + "/" +
                           std::to_string(ok_imgs + bad_imgs);
                parse_res = std::make_shared<RenderResult>(
                    paint_doc_top(parse_res->document, page_css_));
                content_result_ = parse_res;
                adopt_content(parse_res);
            }
        }

        // Indexación incremental REAL: esta visita alimenta el buscador
        if (parse_res->document && parse_res->document->get_body()) {
            std::string text = parse_res->document->get_body()->get_text_content();
            if (text.size() > 40) {
                index_sp_->add_document(current_url_, current_title_, text);
                if (!pages_path_.empty()) index_sp_->save(pages_path_);
            }
        }

        compose();
        rebuild_chrome();
    }

    // ---------------- Formularios REALES ----------------

    void unfocus_webfield() {
        if (caret_el_obs_) { caret_el_obs_->remove_attribute("data-nuby-caret"); caret_el_obs_ = nullptr; }
        web_field_.reset();
    }

    // DFS REAL sobre el DOM: recoge elementos por nombre de etiqueta
    static std::vector<std::shared_ptr<html::Element>> collect_elements(
            std::shared_ptr<html::Node> start, const std::string& tag) {
        // subir a la raíz del documento
        while (start && start->get_parent()) start = start->get_parent();
        std::vector<std::shared_ptr<html::Element>> out;
        std::function<void(std::shared_ptr<html::Node>)> walk = [&](std::shared_ptr<html::Node> n) {
            if (!n) return;
            if (n->is_element() &&
                std::static_pointer_cast<html::Element>(n)->get_tag_name() == tag)
                out.push_back(std::static_pointer_cast<html::Element>(n));
            for (auto& c : n->get_children()) walk(c);
        };
        walk(start);
        return out;
    }

    static std::shared_ptr<html::Element> find_form(std::shared_ptr<html::Element> el) {
        auto p = el ? el->get_parent() : nullptr;
        while (p) {
            if (p->is_element() &&
                std::static_pointer_cast<html::Element>(p)->get_tag_name() == "form")
                return std::static_pointer_cast<html::Element>(p);
            p = p->get_parent();
        }
        return nullptr;
    }

    // application/x-www-form-urlencoded REAL (espacio → '+', resto → %HH)
    static std::string form_encode(const std::string& s) {
        static const char* ok = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._*";
        std::string out;
        char hex[4];
        for (unsigned char c : s) {
            if (c == ' ') out += '+';
            else if (strchr(ok, c)) out += (char)c;
            else { snprintf(hex, sizeof hex, "%%%02X", c); out += hex; }
        }
        return out;
    }

    // Recoge los "successful controls" del formulario (regla HTML real):
    // - con name; checkbox/radio solo si checked; solo el submit presionado viaja
    static std::string collect_form_fields(const std::shared_ptr<html::Element>& form,
                                           const std::shared_ptr<html::Element>& clicked) {
        std::string qs;
        std::function<void(std::shared_ptr<html::Node>)> walk = [&](std::shared_ptr<html::Node> n) {
            if (!n) return;
            if (n->is_element()) {
                auto el = std::static_pointer_cast<html::Element>(n);
                const std::string& tag = el->get_tag_name();
                if (tag == "input" || tag == "textarea" || tag == "button" || tag == "select") {
                    std::string name = el->get_attribute("name");
                    std::string type = core::StringUtils::to_lower(el->get_attribute("type"));
                    if (!name.empty()) {
                        bool include = true;
                        std::string val = el->get_attribute("value");
                        if (type == "checkbox" || type == "radio") {
                            include = el->has_attribute("checked");
                            if (val.empty()) val = "on";
                        } else if (tag == "textarea") {
                            val = el->get_attribute("value"); // edición vive en value (nuestro modelo)
                        } else if (type == "submit" || type == "button" || type == "reset" || tag == "button") {
                            include = (el == clicked);
                        } else if (tag == "select") {
                            include = false; // select no interactivo todavía (honesto)
                        }
                        if (include) {
                            if (!qs.empty()) qs += '&';
                            qs += form_encode(name) + "=" + form_encode(val);
                        }
                    }
                }
            }
            for (auto& c : n->get_children()) walk(c);
        };
        walk(form);
        return qs;
    }

    void submit_form(std::shared_ptr<html::Element> ctrl) {
        auto form = find_form(ctrl);
        if (!form) {
            // HTML real: control sin <form> no envía nada; lo decimos claro
            status_ = "El control no pertenece a un <form> — nada que enviar";
            rebuild_chrome();
            return;
        }
        std::string qs = collect_form_fields(form, ctrl);
        std::string action = core::StringUtils::trim(form->get_attribute("action"));
        std::string method = core::StringUtils::to_lower(core::StringUtils::trim(form->get_attribute("method")));
        unfocus_webfield();
        focus_ = Focus::NONE;

        if (method == "post") {
            std::string target = net::Fetcher::resolve_url(current_url_, action.empty() ? current_url_ : action);
            status_ = "POST " + target + " — " + std::to_string(qs.size()) + " bytes de formulario";
            rebuild_chrome();
            navigate_post(target, qs);
        } else {
            std::string target = net::Fetcher::resolve_url(current_url_, action.empty() ? current_url_ : action);
            if (!qs.empty())
                target += (target.find('?') == std::string::npos ? "?" : "&") + qs;
            navigate(target);
        }
    }

    // reset REAL: restaura value/checked al estado guardado al primer foco.
    // (Modelo honesto: al enfocar guardamos el default en data-nuby-default.)
    void reset_form(std::shared_ptr<html::Element> ctrl) {
        auto form = find_form(ctrl);
        if (!form) return;
        std::function<void(std::shared_ptr<html::Node>)> walk = [&](std::shared_ptr<html::Node> n) {
            if (!n) return;
            if (n->is_element()) {
                auto el = std::static_pointer_cast<html::Element>(n);
                const std::string& tag = el->get_tag_name();
                if (tag == "input" || tag == "textarea") {
                    std::string type = core::StringUtils::to_lower(el->get_attribute("type"));
                    if (type == "checkbox" || type == "radio") {
                        if (el->get_attribute("data-nuby-default-checked") == "1")
                            el->set_attribute("checked", "");
                        else el->remove_attribute("checked");
                    } else if (el->has_attribute("data-nuby-default")) {
                        el->set_attribute("value", el->get_attribute("data-nuby-default"));
                    } else {
                        el->set_attribute("value", "");
                    }
                }
            }
            for (auto& c : n->get_children()) walk(c);
        };
        walk(form);
    }

    // Re-render del documento vivo (tras editar campos/toggles SIN tocar JS).
    // CLAVE: render_document trabaja sobre el MISMO DOM → el foco
    // (web_field_) y el intérprete sobreviven a todos los re-renders.
    void refresh_content() {
        if (!content_result_ || !content_result_->document) { compose(); rebuild_chrome(); return; }
        auto res = std::make_shared<RenderResult>(
            paint_doc_top(content_result_->document,
                          page_css_.empty() ? UA_EXTRA_CSS : page_css_));
        content_result_ = res;
        adopt_content(res);
        // el intérprete ligado a ESTE documento se conserva (render_document)
        auto eng = engine_content_->get_js_engine();
        js_ = eng ? eng->interpreter() : nullptr;
        if (js_) js_->clear_mutation();
        compose();
        rebuild_chrome();
    }

    void record_visit(const std::string& url, const std::string& title) {
        if (url.rfind("nuby://", 0) == 0) return;
        if (!setting_history_) return; // opción REAL: historial desactivado → no se guarda
        HistoryEntry e{url, title, timestamp_now()};
        history_.push_front(e);
        while (history_.size() > 60) history_.pop_back();
    }
    static std::string timestamp_now() {
        time_t t = time(nullptr);
        char buf[32];
        strftime(buf, sizeof buf, "%d %b %Y %H:%M", localtime(&t));
        return buf;
    }

    // ---------------- Páginas internas (todas pintadas por el motor) ----------------
    void go_home() {
        mode_ = Mode::HOME;
        current_url_ = "nuby://home";
        current_title_ = "Nuby — Inicio";
        input_url_ = "nuby://home";
        status_ = "Listo — indice local: " + std::to_string(index_sp_->document_count()) +
                  " docs / " + std::to_string(index_sp_->term_count()) + " terminos (rastreo real)";
        render_home();
        rebuild_chrome();
    }

    void render_home() {
        // HOME NATIVA: este HTML/CSS no se envía como página web al cliente.
        // BrowserShell lo entrega al pipeline propio HTML → CSS → layout →
        // display-list → rasterizador de Nuby y el monitor solo muestra el frame.
        const int side = W >= 680 ? (W - 580) / 2 : 18;
        const int top_gap = H >= 700 ? 112 : (H >= 520 ? 70 : 38);
        const char* ring = (focus_ == Focus::SEARCH) ? "#1a73e8" : "#dadce0";
        // Textos que CABEN en el ancho real: en un móvil la píldora mide ~354 px
        // y una frase larga se partía en dos renglones y empujaba el botón de
        // buscar fuera de la caja (defecto visto en la revisión de móvil).
        const char* placeholder = W < 520 ? "Buscar en Nuby..."
                                          : "Buscar videojuegos, tecnología";
        const bool estrecho = W < 680;
        const int nav_gap = W < 390 ? 6 : (estrecho ? 10 : 14);

        std::ostringstream h;
        h << "<div style=\"background-color: #f8f9fa; height: " << H << "px; padding: 18px " << side
          << "px 14px " << side << "px;\">"
          // Encabezado nativo, con marca y perfil.
          << "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
          << "<span style=\"color: #202124; font-size: 24px; font-weight: 300;\">Nuby</span>"
          << "<div style=\"flex-grow: 1;\"></div>"
          << "<div data-action=\"menu\" style=\"width: 34px; height: 34px; border-radius: 17px; background-color: #e1e3e1;\">"
          << "<div style=\"text-align: center; margin-top: 8px;\"><span style=\"color: #3c4043; font-size: 14px;\">N</span></div></div></div>"
          // Zona central de búsqueda.
          << "<div style=\"margin-top: " << top_gap << "px; text-align: center;\">"
          << "<div style=\"line-height: 68px;\"><span style=\"color: #202124; font-size: 58px; font-weight: 300;\">Nu</span><span style=\"color: #1a73e8; font-size: 58px; font-weight: 400;\">by</span></div>"
          << "<div style=\"margin-top: 28px; background-color: " << ring << "; border-radius: 30px; padding: 1px;\">"
          << "<div data-nuby-input=\"search\" style=\"background-color: #ffffff; border-radius: 29px; padding: 10px 10px 10px 18px;\">"
          << "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
          // Lupa dibujada con primitivas del motor (aro + mango), no un emoji.
          << lupa_html(13, "#5f6368")
          << "<span style=\"color: " << (input_search_.empty() && focus_ != Focus::SEARCH ? "#5f6368" : "#202124")
          << "; font-size: 15px; margin-left: 12px;\">"
          << esc(focus_ == Focus::SEARCH ? input_search_ : (input_search_.empty() ? placeholder : input_search_)) << "</span>";
        if (focus_ == Focus::SEARCH && caret_on_)
            h << "<div style=\"width: 2px; height: 19px; background-color: #1a73e8; margin-left: 2px;\"></div>";
        h << "<div style=\"flex-grow: 1;\"></div>"
          << "<div data-action=\"do-search\" style=\"width: 36px; height: 36px; border-radius: 18px; background-color: #e8f0fe;\">"
          << "<div style=\"margin-top: 9px; margin-left: 9px;\">" << lupa_html(12, "#1a73e8") << "</div></div>"
          << "</div></div></div>"
          << "<div style=\"margin-top: 17px;\"><span style=\"color: #5f6368; font-size: 12px;\">Busca rápidamente en la web con Nuby</span></div></div>"
          // Navegación principal pintada por el motor. Los enlaces usan la
          // navegación interna real y por tanto no son botones decorativos.
          << "<div style=\"margin-top: " << (H >= 700 ? 108 : (H >= 520 ? 64 : 36))
          << "px; background-color: #ffffff; padding: 12px 6px;\">"
          << "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
          << "<a href=\"nuby://home\" style=\"color: #1a73e8; font-size: " << (estrecho ? 12 : 12)
          << "px;\">" << (estrecho ? "" : "⌂ ") << "Inicio</a>"
          << "<div style=\"width: " << nav_gap << "px;\"></div>"
          << "<div data-nuby-input=\"search\" style=\"padding: 2px;\"><span style=\"color: #5f6368; font-size: 12px;\">"
          << "Buscar</span></div>"
          << "<div style=\"width: " << nav_gap << "px;\"></div>"
          << "<a href=\"nuby://history\" style=\"color: #5f6368; font-size: 12px;\">"
          << (estrecho ? "" : "★ ") << "Historial</a>"
          << "<div style=\"width: " << nav_gap << "px;\"></div>"
          << "<a href=\"nuby://settings\" style=\"color: #5f6368; font-size: 12px;\">"
          << (estrecho ? "" : "⚙ ") << "Config.</a>"
          << "<div style=\"width: " << nav_gap << "px;\"></div>"
          << "<a href=\"nuby://menu\" style=\"color: #5f6368; font-size: 12px;\">"
          << (estrecho ? "" : "☰ ") << "Menú</a>"
          << "</div></div>"
          << "</div>";
        render_content_doc(h.str(), UI_CSS, {});
    }

    void show_search_results(const std::string& q) {
        mode_ = Mode::RESULTS;
        search_of_ = q;
        current_url_ = "nuby://search?q=" + url_encode(q);
        input_url_ = current_url_;
        current_title_ = q + " - Buscar";

        auto t0 = std::chrono::steady_clock::now();
        last_hits_ = index_sp_->query(q, 24); // consulta REAL → caché de ESTA sesión
        last_query_secs_ = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        render_results_display();
    }

    // Pinta resultados desde la caché REAL (sin re-consultar el índice).
    // La usa la opción "búsqueda instantánea: desactivada": al teclear solo
    // se actualiza la caja; BM25 vuelve a correr en el próximo Enter.
    void render_results_display() {
        const std::string& q = search_of_;
        const auto& hits = last_hits_;
        double secs = last_query_secs_;
        status_ = std::to_string(hits.size()) + " resultados";

        const int pad = (W >= 720) ? 60 : 18;
        std::ostringstream h;
        h << "<div style=\"padding: 18px " << pad << "px 40px " << pad << "px;\">"
          // fila superior: mini-logo + caja con la query (como todo buscador).
          // En móvil el logo se omite: la caja ya ocupa todo el ancho útil y el
          // logo se montaba encima del texto (defecto visto en la revisión).
             "<div style=\"display: flex; flex-direction: row; align-items: center; gap: 14px; margin-bottom: 10px;\">";
        if (W >= 640)
            h <<   "<div style=\"line-height: 30px;\"><a href=\"nuby://home\" style=\"color: #1a73e8; font-size: 20px; font-weight: 800;\">Nuby</a></div>";
        h << ""
               "<div data-nuby-input=\"search\" style=\"flex-grow: 1; max-width: 560px; border: 1px solid #dfe1e5; border-radius: 20px; padding: 8px 14px;\">"
                 "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
                 "<span style=\"color: #202124; font-size: 15px;\">"
          << esc(focus_ == Focus::SEARCH ? input_search_ : q) << "</span>";
        if (focus_ == Focus::SEARCH && caret_on_)
            h << "<div style=\"width: 2px; height: 18px; background-color: #1a73e8; margin-left: 2px;\"></div>";
        h <<   "</div></div></div>"
          // conteo honesto estilo Google: dentro de la página, gris, pequeño
             "<p style=\"color: #70757a; font-size: 12px; margin-bottom: 20px;\">Cerca de "
          << std::to_string(hits.size()) << " resultados (" << secs_str(secs) << " segundos)</p>";

        if (hits.empty()) {
            h << "<p style=\"color: #202124; font-size: 15px;\">No se encontraron resultados para «"
              << esc(q) << "».</p>"
                 "<p style=\"color: #4d5156; font-size: 13px;\">Prueba con otras palabras, o navega a sitios "
                 "web: cada pagina que abras se indexa automaticamente.</p>";
        }
        for (const auto& hit : hits) {
            // layout de resultado de buscador serio: URL breadcrumbs arriba,
            // título azul, snippet gris. Sin scores ni cajas técnicas.
            h << "<div style=\"margin-bottom: 24px;\">"
                 "<div style=\"margin-bottom: 2px;\">"
                   "<span style=\"color: #202124; font-size: 12px;\">" << esc(hit.doc->domain) << "</span>"
                   "<span style=\"color: #5f6368; font-size: 12px;\"> › " << esc(crumb_of(hit.doc->url)) << "</span>"
                 "</div>"
                 "<a href=\"" << esc(hit.doc->url) << "\" style=\"color: #1a0dab; font-size: 17px; font-weight: 700;\">"
              << esc(hit.doc->title) << "</a>"
                 "<p style=\"color: #4d5156; font-size: 13px; margin-top: 3px;\">" << esc(hit.snippet) << "</p>"
              "</div>";
        }
        h << "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    static std::string secs_str(double s) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.4f", s);
        return buf;
    }

    // breadcrumb honesto: sección principal del path (como Google hoy):
    // ja.wikipedia.org/wiki/… → "wiki". Nada de percent-encodings crudos.
    static std::string crumb_of(const std::string& url) {
        auto pos = url.find("://");
        std::string rest = pos != std::string::npos ? url.substr(pos + 3) : url;
        auto slash = rest.find('/');
        if (slash == std::string::npos || slash + 1 >= rest.size()) return "inicio";
        std::string path = rest.substr(slash + 1);
        auto end = path.find('/');
        std::string first = end == std::string::npos ? path : path.substr(0, end);
        if (first.empty()) return "inicio";
        if (first.size() > 24) first = first.substr(0, 23) + "…";
        return first;
    }

    void show_error(const std::string& url, const std::string& err) {
        mode_ = Mode::ERROR_PAGE;
        current_title_ = "Error";
        status_ = "Fallo de navegacion";
        const int pad_e = (W >= 720) ? 70 : 24;
        std::ostringstream h;
        h << "<div style=\"padding: 60px " << pad_e << "px;\">"
             "<h1 style=\"color: #b3261e; font-size: 26px; margin: 0 0 14px 0;\">No se pudo abrir la pagina</h1>"
             "<p style=\"color: #333333; font-size: 15px; margin-bottom: 8px;\"><strong>" << esc(url) << "</strong></p>"
             "<p style=\"color: #555555; font-size: 14px; margin-bottom: 22px;\">" << esc(err) << "</p>"
             "<p style=\"color: #555555; font-size: 13px;\">Este mensaje tambien lo pinta el motor Nuby. "
             "Si estas en el sandbox de Arena, recuerda: no hay salida a internet; en tu PC/Termux la misma peticion es real.</p>"
             "<div style=\"margin-top: 20px;\"><a href=\"nuby://home\" style=\"color: #0b57d0; font-size: 14px;\">volver al inicio</a></div>"
          "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    void show_about(const std::string& back_to) {
        mode_ = Mode::ABOUT;
        about_back_ = back_to; // para re-generar bien tras un resize
        current_url_ = "nuby://about";
        input_url_ = "nuby://about";
        current_title_ = "Nuby — la verdad tecnica";
        status_ = "nuby://about";
        const int pad_about = (W >= 720) ? 60 : 20;
        std::ostringstream h;
        h << "<div style=\"padding: 30px " << pad_about << "px;\">"
             "<h1 style=\"color: #111111; font-size: 28px; margin: 0 0 12px 0;\">Que es real aqui (y que no)</h1>"
             "<p style=\"font-size: 14px; color: #333;\">Esta pagina, la barra de arriba y todo lo que ves "
             "lo calcularon el parser HTML, la cascada CSS, el motor de layout y el rasterizador de Nuby. "
             "Tu navegador solo muestra los pixeles, como un monitor.</p>"
             "<h2 style=\"font-size: 19px; margin: 18px 0 6px 0;\">REAL en esta build</h2>"
             "<p style=\"font-size: 13px; color: #222;\">"
             "· Parser HTML, CSS (cascada + especificidad real), rasterizador propio AA<br>"
             "· Layout: bloques + flex 3-pasos + <b>IFC real</b> (lineas inline con wrap, baseline y text-align)<br>"
             "· Imagenes: <b>decodificador PNG propio</b> (DEFLATE, filtros, paleta, alfa) — pintadas pixel a pixel<br>"
             "· Formularios: inputs con caret, checkbox/radio, GET y POST de verdad (probados contra servidor eco)<br>"
             "· Multi-sesion: cada visitante tiene su propio navegador aislado por cookie; indice BM25 compartido<br>"
             "· Viewport dinamico: el motor renderiza AL TAMANO real de tu pantalla (movil o escritorio)<br>"
             "· Red: DNS+TCP+HTTP/1.1 propios; HTTPS via TLS real del OpenSSL del sistema; dechunking real<br>"
             "· Buscador: indice invertido + BM25 propios (" << std::to_string(index_sp_->document_count())
          << " docs, " << std::to_string(index_sp_->term_count()) << " terminos, rastreo real del 8-ago-2026)<br>"
             "· JavaScript: interprete real (lexer, parser, AST, closures) para un subconjunto documentado<br>"
             "· Historial, atras/adelante, e indexacion incremental al navegar: todo en memoria real</p>"
             "<h2 style=\"font-size: 19px; margin: 18px 0 6px 0;\">NO soportado todavia (la verdad)</h2>"
             "<p style=\"font-size: 13px; color: #222;\">"
             "· JPEG y GIF: placeholder HONESTO con la razon (solo PNG por ahora)<br>"
             "· &lt;select&gt; no es interactivo; PNG entrelazado Adam7 reporta error explicito<br>"
             "· gzip/br, tablas CSS, position:sticky, fuentes TTF (usa bitmap 8x12), JS moderno completo<br>"
             "· Webs gigantes con JS pesado no funcionaran; nadie construye un Chrome en una semana</p>"
             "<div style=\"margin-top: 22px;\"><a href=\"" << esc(back_to) << "\" style=\"color: #0b57d0; font-size: 14px;\">volver</a></div>"
          "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    void show_history() {
        mode_ = Mode::HISTORY;
        current_url_ = "nuby://history";
        input_url_ = current_url_;
        current_title_ = "Historial";
        status_ = std::to_string(history_.size()) + " visitas reales en esta sesion";
        const int pad_h = (W >= 720) ? 60 : 20;
        std::ostringstream h;
        h << "<div style=\"padding: 26px " << pad_h << "px;\">"
             "<h1 style=\"color: #111111; font-size: 26px; margin: 0 0 14px 0;\">Historial de navegacion</h1>";
        if (history_.empty()) {
            h << "<p style=\"color: #555; font-size: 14px;\">Todavia no has visitado ninguna pagina web real.</p>";
        }
        for (const auto& e : history_) {
            h << "<div style=\"margin-bottom: 10px;\">"
                 "<span style=\"color: #888888; font-size: 12px;\">" << esc(e.when) << " — </span>"
                 "<a href=\"" << esc(e.url) << "\" style=\"color: #0b57d0; font-size: 14px;\">" << esc(e.title) << "</a>"
              "</div>";
        }
        h << "<div style=\"margin-top: 18px;\">"
             "<a href=\"nuby://clear-history\" style=\"color: #b3261e; font-size: 13px; font-weight: 700;\">vaciar historial</a>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://menu\" style=\"color: #0b57d0; font-size: 13px;\">menú</a>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://home\" style=\"color: #0b57d0; font-size: 13px;\">inicio</a></div>"
          "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    // ---------------- ☰ Menú (cajón lateral pintado por el motor) ----------
    void show_menu() {
        mode_ = Mode::MENU;
        current_url_ = "nuby://menu";
        input_url_ = current_url_;
        current_title_ = "Menú";
        status_ = "Menú";
        const int panel_w = std::min(320, W - 56);
        const int panel_h = std::max(430, visible_h());
        const std::string sep =
            "<div style=\"height: 1px; background-color: #ececec; margin: 6px 0;\"></div>";
        std::ostringstream h;
        auto item = [&](const std::string& label, const std::string& href2,
                        const std::string& sub) {
            h << "<a href=\"" << href2 << "\" style=\"display: block; padding: 9px 20px;\">"
                 "<span style=\"color: #202124; font-size: 15px;\">" << label << "</span>";
            if (!sub.empty())
                h << "<div style=\"margin-top: 2px;\"><span style=\"color: #80868b; font-size: 11px;\">"
                  << esc(sub) << "</span></div>";
            h << "</a>";
        };

        h << "<div style=\"display: flex; flex-direction: row;\">"
             "<div style=\"width: " << panel_w << "px; height: " << panel_h << "px; background-color: #ffffff;\">"
             "<div style=\"padding: 16px 20px 8px 20px;\">"
               "<span style=\"color: #202124; font-size: 24px; font-weight: 800;\">Nuby</span>"
             "</div>"
             "<div style=\"margin: 0 20px 8px 20px; padding: 5px 10px; background-color: #f1f3f4; border-radius: 10px;\">"
               "<span style=\"color: #5f6368; font-size: 11px;\">motor C++ · índice BM25 · "
          << index_sp_->document_count() << " páginas reales</span>"
             "</div>"
          << sep;
        item("Historial", "nuby://history",
             std::to_string(history_.size()) + " visitas reales en esta sesión");
        item("Descargas", "nuby://downloads",
             std::to_string(downloads_.size()) + " páginas bajadas por el motor");
        item("Configuración", "nuby://settings", "texto, cursor, historial…");
        h << sep;
        item("Acerca de Nuby", "nuby://about", "la verdad técnica del motor");
        h << sep;
        item("Cerrar menú", "nuby://home", "");
        h <<   "</div>"
             "<div style=\"width: 1px; background-color: #e0e0e0;\"></div>"
             "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    // ---------------- Descargas (registro REAL de red de esta sesión) ------
    void show_downloads() {
        mode_ = Mode::DOWNLOADS;
        current_url_ = "nuby://downloads";
        input_url_ = current_url_;
        current_title_ = "Descargas";
        status_ = std::to_string(downloads_.size()) + " descargas reales en esta sesión";
        const int pad = (W >= 720) ? 60 : 20;
        std::ostringstream h;
        h << "<div style=\"padding: 26px " << pad << "px;\">"
             "<h1 style=\"color: #111111; font-size: 26px; margin: 0 0 6px 0;\">Descargas</h1>"
             "<p style=\"color: #5f6368; font-size: 12px; margin-bottom: 16px;\">Páginas web que el motor "
             "ha descargado de la red en ESTA sesión, con sus bytes exactos. "
             "Aquí no hay entradas de muestra: si está vacío es que aún no se ha bajado nada.</p>";
        if (downloads_.empty()) {
            h << "<p style=\"color: #555; font-size: 14px;\">Aún no hay descargas reales. "
                 "Navega a una página web y aparecerá aquí.</p>";
        }
        for (const auto& d : downloads_) {
            std::string kb = d.bytes >= 1024
                ? std::to_string(d.bytes / 1024) + " KB"
                : std::to_string(d.bytes) + " B";
            h << "<div style=\"margin-bottom: 14px;\">"
                 "<a href=\"" << esc(d.url) << "\" style=\"color: #0b57d0; font-size: 14px;\">"
              << esc(d.title.empty() ? d.url : d.title) << "</a>"
                 "<div style=\"margin-top: 1px;\">"
                   "<span style=\"color: #188038; font-size: 12px; font-weight: 700;\">" << kb << "</span>"
                   "<span style=\"color: #888888; font-size: 12px;\"> · " << esc(d.when)
              << " · " << esc(d.url) << "</span>"
                 "</div>"
              "</div>";
        }
        h << "<div style=\"margin-top: 18px;\">"
             "<a href=\"nuby://clear-downloads\" style=\"color: #b3261e; font-size: 13px; font-weight: 700;\">vaciar lista</a>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://menu\" style=\"color: #0b57d0; font-size: 13px;\">menú</a>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://home\" style=\"color: #0b57d0; font-size: 13px;\">inicio</a></div>"
          "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    // ---------------- Configuración (cada opción APLICA un cambio real) ----
    void apply_setting(const std::string& optval) {
        auto slash = optval.find('/');
        std::string key = slash == std::string::npos ? optval : optval.substr(0, slash);
        std::string val = slash == std::string::npos ? "" : optval.substr(slash + 1);
        if (key == "textzoom") {
            float z = (val == "0") ? 0.90f : (val == "2") ? 1.25f : 1.0f;
            paint::FontRasterizer::set_text_zoom(z); // cambia MEDIDA y TRAZADO (real, global)
            status_ = "Zoom de texto aplicado: " + std::to_string((int)(paint::FontRasterizer::text_zoom() * 100)) + "%";
        } else if (key == "blink") {
            setting_blink_ = (val == "1");
            status_ = std::string("Cursor parpadeante: ") + (setting_blink_ ? "activado" : "desactivado");
        } else if (key == "instant") {
            setting_instant_ = (val == "1");
            status_ = std::string("Búsqueda instantánea: ") + (setting_instant_ ? "activada" : "desactivada");
        } else if (key == "history") {
            setting_history_ = (val == "1");
            status_ = std::string("Guardar historial: ") + (setting_history_ ? "activado" : "desactivado");
        } else {
            status_ = "Opción desconocida: " + key;
        }
        // Re-render HONESTO: la geometría pudo cambiar (zoom) → regenerar todo
        show_settings();
    }

    void show_settings() {
        mode_ = Mode::SETTINGS;
        current_url_ = "nuby://settings";
        input_url_ = current_url_;
        current_title_ = "Configuración";
        const int pad = (W >= 720) ? 60 : 20;
        const float z = paint::FontRasterizer::text_zoom();
        std::ostringstream h;
        std::string cur_key; // clave de la opción en curso (la usa el lambda choice)

        auto row_head = [&](const std::string& label, const std::string& sub) {
            h << "<div style=\"margin-bottom: 4px;\">"
                 "<span style=\"color: #202124; font-size: 15px;\">" << label << "</span>"
                 "<div style=\"margin-top: 1px;\"><span style=\"color: #80868b; font-size: 11px;\">"
              << esc(sub) << "</span></div></div>";
        };
        auto choice = [&](const std::string& val, const std::string& label, bool current) {
            if (current)
                h << "<span style=\"color: #202124; font-size: 13px; font-weight: 700;\">  " << label << " ✓  </span>";
            else
                h << "<a href=\"nuby://set/" << cur_key << "/" << val
                  << "\" style=\"color: #0b57d0; font-size: 13px;\">  " << label << "  </a>";
        };

        h << "<div style=\"padding: 26px " << pad << "px;\">"
             "<h1 style=\"color: #111111; font-size: 26px; margin: 0 0 6px 0;\">Configuración</h1>"
             "<p style=\"color: #5f6368; font-size: 12px; margin-bottom: 18px;\">Cada opción cambia "
             "comportamiento REAL del motor en el momento. Nada aquí es decorativo.</p>";

        // 1) Tamaño de texto — zoom global real (medida + trazado)
        cur_key = "textzoom";
        row_head("Tamaño de texto", "Zoom global del motor: afecta al layout, no es un filtro.");
        choice("0", "Pequeño", z < 0.95f);
        choice("1", "Normal", z >= 0.95f && z < 1.15f);
        choice("2", "Grande", z >= 1.15f);
        h << "<div style=\"height: 1px; background-color: #ececec; margin: 14px 0;\"></div>";

        // 2) Cursor parpadeante
        cur_key = "blink";
        row_head("Cursor parpadeante", "Si lo apagas, el cursor queda fijo y el motor deja de re-renderizar cada 550 ms.");
        choice("1", "Activado", setting_blink_);
        choice("0", "Desactivado", !setting_blink_);
        h << "<div style=\"height: 1px; background-color: #ececec; margin: 14px 0;\"></div>";

        // 3) Búsqueda instantánea
        cur_key = "instant";
        row_head("Búsqueda instantánea", "Activada: BM25 corre por tecla. Apagada: la búsqueda corre solo al pulsar Enter.");
        choice("1", "Activada", setting_instant_);
        choice("0", "Desactivada", !setting_instant_);
        h << "<div style=\"height: 1px; background-color: #ececec; margin: 14px 0;\"></div>";

        // 4) Guardar historial
        cur_key = "history";
        row_head("Guardar historial", "Apagado: las visitas dejan de registrarse (privacidad real, verificable en Historial).");
        choice("1", "Activado", setting_history_);
        choice("0", "Desactivado", !setting_history_);

        h << "<div style=\"margin-top: 22px;\">"
             "<a href=\"nuby://menu\" style=\"color: #0b57d0; font-size: 13px;\">menú</a>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://home\" style=\"color: #0b57d0; font-size: 13px;\">inicio</a></div>"
          "</div>";
        render_content_doc(h.str(), UI_CSS, {});
        rebuild_chrome();
    }

    // CSS extra del agente de usuario (real, encima de la hoja UA del motor)
    inline static const std::string UA_EXTRA_CSS = R"CSS(
        head, style, script, title, meta, link, noscript { display: none; }
        html, body { margin: 0; padding: 0; }
        body { margin: 0px; background-color: #ffffff; color: #1a1a1a; font-size: 15px; line-height: 22px; }
        a { color: #0b57d0; }
        b, strong { font-weight: 700; }
        h1 { font-size: 28px; font-weight: 700; margin: 12px 0; }
        h2 { font-size: 22px; font-weight: 700; margin: 10px 0; }
        h3 { font-size: 18px; font-weight: 700; margin: 8px 0; }
        p { margin: 8px 0; }
        ul, ol { margin: 8px 0 8px 22px; }
        li { display: block; margin: 3px 0; }
        hr { border-top-width: 1px; border-color: #888888; margin: 12px 0; }
        blockquote { margin: 10px 18px; color: #555555; }
        td, th { display: inline-block; padding: 4px 8px; }
        tr { display: block; }
        table { display: block; border-top-width: 1px; border-color: #cccccc; margin: 8px 0; }
        [data-nuby-ph] { display: inline-block; background-color: #ececec; color: #666666; border: 1px solid #cccccc; border-radius: 4px; padding: 2px 6px; font-size: 12px; margin: 1px; }
        input, textarea, select, button { display: inline-block; background-color: #ffffff; color: #1a1a1a; border: 1px solid #9aa0a6; border-radius: 4px; padding: 3px 6px; font-size: 15px; line-height: 20px; margin: 2px 0; }
        input { width: 220px; height: 26px; }
        textarea { width: 380px; height: 72px; }
        input[type="checkbox"], input[type="radio"] { width: 14px; height: 14px; padding: 2px; }
        input[type="radio"] { border-radius: 9px; }
        input[type="submit"], input[type="button"], input[type="reset"], button { width: 120px; height: 30px; background-color: #e8f0fe; color: #0b57d0; border: 1px solid #aec7ee; border-radius: 6px; }
        input[type="hidden"], input[type="file"], input[type="image"] { display: none; }
        img { display: inline-block; }
        img[data-nuby-imgfail] { border: 1px dashed #9aa0a6; border-radius: 4px; }
    )CSS";

    // frame compuesto
    std::shared_ptr<RenderResult> chrome_result_;
    std::vector<uint32_t> chrome_fb_;
    std::string content_base_url_;
};

} // namespace nuby::app
