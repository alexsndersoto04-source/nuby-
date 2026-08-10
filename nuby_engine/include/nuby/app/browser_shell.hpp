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
#include "../js/js_interp.hpp"
#include "../media/png_decoder.hpp"
#include "html_preprocess.hpp"
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include <sstream>

namespace nuby::app {

struct HistoryEntry {
    std::string url;
    std::string title;
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

    // Re-renderiza TODO a un nuevo tamaño de ventana (móvil ↔ escritorio).
    void set_viewport(int w, int h) {
        // caps honestos: ni micro-frames ni monstruos que revienten la RAM
        w = std::max(240, std::min(1920, w));
        h = std::max(320, std::min(2160, h));
        if (w == W && h == H) return;
        W = w; H = h;
        engine_chrome_->set_viewport(W, CHROME_H);
        engine_content_->set_viewport(W, CONTENT_CAP);
        scroll_y_ = 0; // el alto visible cambió; el scroll viejo no aplica
        // Las páginas INTERNAS hornean medidas del viewport en su markup
        // (centrado por padding, caja responsiva): con un resize hay que
        // RE-GENERARLAS, no solo re-maquetar el HTML viejo (bug real: la
        // home quedaba centrada para el ancho anterior).
        switch (mode_) {
            case Mode::HOME: render_home(); break;
            case Mode::RESULTS:
                if (!search_of_.empty()) show_search_results(search_of_);
                else render_home();
                break;
            case Mode::ABOUT: show_about(about_back_); break;
            case Mode::HISTORY: show_history(); break;
            default: refresh_content(); break; // páginas web reales: mismo DOM, nuevo ancho
        }
        rebuild_chrome(); // incluye compose() del frame final
    }
    static constexpr int CONTENT_CAP = 8000; // tope de alto de página (seguridad)

    // Multi-sesión REAL: varias sesiones comparten UN índice de búsqueda
    // (como un crawler colectivo), pero cada una tiene su propio navegador:
    // historial, página actual, cookies de foco — todo aislado por sesión.
    explicit BrowserShell(std::shared_ptr<search::SearchIndex> shared_index = nullptr)
        : index_sp_(std::move(shared_index)) {
        if (!index_sp_) index_sp_ = std::make_shared<search::SearchIndex>();
        engine_chrome_ = std::make_unique<NubyBrowserEngine>(W, CHROME_H);
        engine_content_ = std::make_unique<NubyBrowserEngine>(W, CONTENT_CAP);
        go_home();
    }

    void set_data_path(const std::string& pages_tsv) { pages_path_ = pages_tsv; }
    search::SearchIndex& index() { return *index_sp_; }

    // ---------------- Entrada de usuario ----------------

    bool handle_click(int x, int y) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (y < CHROME_H) {
            return click_chrome(x, y);
        }
        return click_content(x, y - CHROME_H + scroll_y_);
    }

    bool handle_wheel(int dy) {
        std::lock_guard<std::mutex> lock(mutex_);
        int max_scroll = std::max(0, content_height_ - (H - CHROME_H));
        int old = scroll_y_;
        scroll_y_ = std::max(0, std::min(max_scroll, scroll_y_ + dy));
        if (scroll_y_ != old) { compose(); return true; }
        return false;
    }

    bool handle_char(uint32_t codepoint) {
        std::lock_guard<std::mutex> lock(mutex_);
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
        else if (focus_ == Focus::SEARCH && mode_ == Mode::RESULTS)
            show_search_results(input_search_); // Instant Search real: BM25 en μs por tecla
        rebuild_chrome();
        return true;
    }

    bool handle_key(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
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
            else if (focus_ == Focus::SEARCH && mode_ == Mode::RESULTS)
                show_search_results(input_search_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (focus_ == Focus::NONE) return false;
        bool on = (now_ms() / 550) % 2 == 0;
        if (on == caret_on_) return false;
        caret_on_ = on;
        rebuild_chrome();
        return true;
    }

    // Repinta la home tras cargar el índice (la primera salió a 0 docs)
    void reload_home() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode_ == Mode::HOME) { go_home(); }
    }

    // ---------------- Búsqueda pública (para la API) ----------------
    std::vector<search::SearchHit> search(const std::string& q) {
        return index_sp_->query(q, 24);
    }
    const std::deque<HistoryEntry>& history() const { return history_; }

    // Navegación pública (para /api/goto): misma vía real que teclear la URL
    void go(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        navigate(url);
        dirty_ = true;
    }
    std::string current_url() { std::lock_guard<std::mutex> lock(mutex_); return current_url_; }
    std::string status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }

private:
    enum class Focus { NONE, URL, SEARCH, WEBFIELD }; // WEBFIELD: <input>/<textarea> REALES de la página
    enum class Mode { HOME, WEB, RESULTS, ERROR_PAGE, ABOUT, HISTORY };

    // ---------------- Estado ----------------
    std::mutex mutex_;
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
    int content_height_{H - CHROME_H};
    bool caret_on_{true};

    std::vector<uint32_t> frame_;       // W*H compuesto
    std::vector<uint32_t> content_fb_;  // W*CONTENT_CAP render de la página
    bool dirty_{true};

    std::vector<std::string> back_stack_;
    std::vector<std::string> fwd_stack_;
    std::deque<HistoryEntry> history_;

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
                try {
                    r += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
                    i += 2;
                    continue;
                } catch (...) {}
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
        // scroll horizontal honesto: se ven los últimos caracteres
        size_t max_vis = (size_t)std::max(24, W / 9); // ~9px/char a 15px
        if (shown.size() > max_vis) shown = "…" + shown.substr(shown.size() - max_vis);

        std::string caret;
        if (url_focused && caret_on_)
            caret = "<div style=\"width: 2px; height: 20px; background-color: #1a73e8; margin-left: 1px;\"></div>";

        std::string lock;
        if (current_url_.rfind("https://", 0) == 0)
            lock = "<span style=\"color: #188038; font-size: 12px; font-weight: 700;\">TLS </span>";
        else if (current_url_.rfind("http://", 0) == 0)
            lock = "<span style=\"color: #b45309; font-size: 12px; font-weight: 700;\">http </span>";

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
        if (W >= 640) // en móvil el espacio es oro: sin logo en el chrome
            h << "<div data-action=\"home\" style=\"margin-right: 4px;\">"
                 "<span style=\"color: #1a73e8; font-size: 18px; font-weight: 800;\">Nuby</span></div>";
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
        if (frame_.size() != (size_t)(W * H)) frame_.assign((size_t)W * H, 0xFFFFFFFF);
        if (chrome_fb_.size() != (size_t)(W * CHROME_H)) rebuild_chrome_only();
        // 1) chrome
        std::copy(chrome_fb_.begin(), chrome_fb_.end(), frame_.begin());
        // 2) contenido (slice con scroll)
        int visible = H - CHROME_H;
        for (int row = 0; row < visible; ++row) {
            int src_row = row + scroll_y_;
            auto dst = frame_.begin() + (size_t)(CHROME_H + row) * W;
            if (src_row >= 0 && src_row < content_height_ &&
                (size_t)((src_row + 1) * W) <= content_fb_.size()) {
                auto src = content_fb_.begin() + (size_t)src_row * W;
                std::copy(src, src + W, dst);
            } else {
                std::fill(dst, dst + W, 0xFFFFFFFF);
            }
        }
        dirty_ = true;
    }
    void rebuild_chrome_only() {
        auto res = engine_chrome_->render_page(chrome_html());
        chrome_result_ = std::make_shared<RenderResult>(std::move(res));
        chrome_fb_ = chrome_result_->pixels;
    }

    // ---------------- Render del documento de contenido ----------------
    void render_content_doc(const std::string& html_clean, const std::string& css,
                            const std::vector<std::string>& scripts) {
        page_css_ = UA_EXTRA_CSS + "\n" + css;
        auto res = std::make_shared<RenderResult>(
            engine_content_->render_page(html_clean, page_css_));
        content_result_ = res;
        content_fb_ = res->pixels;

        // Altura real del contenido: camina el layout tree
        content_height_ = H - CHROME_H;
        if (res->layout_tree) {
            float max_bottom = 0;
            walk_bottom(res->layout_tree, max_bottom);
            content_height_ = std::max(H - CHROME_H, std::min((int)max_bottom + 12, CONTENT_CAP));
        }

        // Ejecuta los scripts inline con el intérprete REAL
        js_ = std::make_shared<js::Interpreter>(res->document);
        bool mutated = false;
        for (auto& code : scripts) {
            try {
                js_->run(code);
            } catch (const std::exception& e) {
                js_->clear_mutation();
                status_ = std::string("JS detenido: ") + e.what();
                break;
            }
            if (js_->dom_mutated()) { mutated = true; js_->clear_mutation(); }
        }
        if (mutated) {
            // El DOM cambió de verdad → repinta con el DOM mutado
            res = std::make_shared<RenderResult>(
                engine_content_->render_page(serialize_body(res->document), UA_EXTRA_CSS + "\n" + css));
            content_result_ = res;
            content_fb_ = res->pixels;
            if (res->layout_tree) {
                float max_bottom = 0;
                walk_bottom(res->layout_tree, max_bottom);
                content_height_ = std::max(H - CHROME_H, std::min((int)max_bottom + 12, CONTENT_CAP));
            }
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

        // Botón "Buscar con Nuby" (mismo camino que Enter: cero duplicación)
        if (el->get_attribute("data-action") == "do-search") {
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
        }

        // Enlaces reales
        if (el->get_tag_name() == "a" && el->has_attribute("href")) {
            std::string href = el->get_attribute("href");
            if (href.rfind("javascript:", 0) == 0) {
                if (js_) {
                    try { js_->run(href.substr(11)); render_after_js(); }
                    catch (const std::exception& e) { status_ = std::string("JS: ") + e.what(); rebuild_chrome(); }
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
                engine_content_->render_document(content_result_->document,
                                                 page_css_.empty() ? UA_EXTRA_CSS : page_css_));
            content_result_ = res;
            content_fb_ = res->pixels;
            // re-bind (mismo documento → mismo intérprete conservado)
            {
                auto eng = engine_content_->get_js_engine();
                js_ = eng ? eng->interpreter() : nullptr;
                if (js_) js_->clear_mutation();
            }
            if (res->layout_tree) {
                float max_bottom = 0;
                walk_bottom(res->layout_tree, max_bottom);
                content_height_ = std::max(H - CHROME_H, std::min((int)max_bottom + 12, CONTENT_CAP));
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

        // Guarda la URL base resuelta para los enlaces relativos
        content_base_url_ = pp.base_href.empty() ? current_url_ : net::Fetcher::resolve_url(current_url_, pp.base_href);

        // Render + scripts (render_content_doc también mide altura real)
        content_result_ = parse_res;
        content_fb_ = parse_res->pixels;
        content_height_ = H - CHROME_H;
        if (parse_res->layout_tree) {
            float mb = 0; walk_bottom(parse_res->layout_tree, mb);
            content_height_ = std::max(H - CHROME_H, std::min((int)mb + 12, CONTENT_CAP));
        }

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
                    engine_content_->render_document(parse_res->document, page_css_));
                content_result_ = parse_res;
                content_fb_ = parse_res->pixels;
                if (parse_res->layout_tree) {
                    float mb2 = 0; walk_bottom(parse_res->layout_tree, mb2);
                    content_height_ = std::max(H - CHROME_H, std::min((int)mb2 + 12, CONTENT_CAP));
                }
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
            engine_content_->render_document(content_result_->document,
                                             page_css_.empty() ? UA_EXTRA_CSS : page_css_));
        content_result_ = res;
        content_fb_ = res->pixels;
        if (res->layout_tree) {
            float mb = 0; walk_bottom(res->layout_tree, mb);
            content_height_ = std::max(H - CHROME_H, std::min((int)mb + 12, CONTENT_CAP));
        }
        // el intérprete ligado a ESTE documento se conserva (render_document)
        auto eng = engine_content_->get_js_engine();
        js_ = eng ? eng->interpreter() : nullptr;
        if (js_) js_->clear_mutation();
        compose();
        rebuild_chrome();
    }

    void record_visit(const std::string& url, const std::string& title) {
        if (url.rfind("nuby://", 0) == 0) return;
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
        // Home limpia estilo buscador profesional: logo, caja, un botón.
        // NADA de texto técnico decorativo — ningún buscador real lo lleva.
        // max-width no existe en el motor: la caja se centra con el padding
        // lateral calculado desde el ancho REAL de la sesión (honesto).
        const int pad = std::max(18, (W - 620) / 2);
        const char* sb_border = (focus_ == Focus::SEARCH) ? "#1a73e8" : "#dadce0";
        std::ostringstream h;
        h << "<div style=\"padding: 120px " << pad << "px 40px " << pad << "px;\">"

          // Logo con colores de marca (como los buscadores grandes)
             "<div style=\"text-align: center; margin-bottom: 34px;\">"
               "<span style=\"color: #4285F4; font-size: 60px; font-weight: 800;\">N</span>"
               "<span style=\"color: #EA4335; font-size: 60px; font-weight: 800;\">u</span>"
               "<span style=\"color: #FBBC05; font-size: 60px; font-weight: 800;\">b</span>"
               "<span style=\"color: #34A853; font-size: 60px; font-weight: 800;\">y</span>"
             "</div>"

          // Caja de búsqueda pill (omnibox: busca O navega)
             "<div data-nuby-input=\"search\" style=\"border: 1px solid " << sb_border
          << "; border-radius: 24px; padding: 12px 18px; background-color: #ffffff;\">"
               "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
               "<span style=\"color: #9aa0a6; font-size: 16px; font-weight: 700;\">Q&nbsp;&nbsp;</span>"
               "<span style=\"color: " << (input_search_.empty() && focus_ != Focus::SEARCH ? "#9aa0a6" : "#202124")
          << "; font-size: 16px;\">"
          << esc(focus_ == Focus::SEARCH ? input_search_
                 : (input_search_.empty() ? "Buscar o escribir una URL" : input_search_))
          << "</span>";
        if (focus_ == Focus::SEARCH && caret_on_)
            h << "<div style=\"width: 2px; height: 20px; background-color: #1a73e8; margin-left: 2px;\"></div>";
        h << "</div></div>"

          // Botón buscar (Enter también sirve; ambos caminos corren el mismo código)
             "<div style=\"text-align: center; margin-top: 26px;\">"
               "<div data-action=\"do-search\" style=\"display: inline-block; width: 160px; text-align: center; background-color: #f8f9fa; border: 1px solid #dfe1e5; border-radius: 6px; padding: 8px 0px;\">"
                 "<span style=\"color: #3c4043; font-size: 14px;\">Buscar con Nuby</span></div>"
             "</div>"

          // Footer funcional discreto (enlaces, no pancartas)
             "<div style=\"text-align: center; margin-top: 120px;\">"
               "<a href=\"nuby://about\" style=\"color: #70757a; font-size: 12px;\">Acerca de Nuby</a>"
               "<span style=\"color: #dadce0; font-size: 12px;\">  ·  </span>"
               "<a href=\"nuby://history\" style=\"color: #70757a; font-size: 12px;\">Historial</a>"
             "</div>"
          "</div>";
        render_content_doc(h.str(), "", {});
    }

    void show_search_results(const std::string& q) {
        mode_ = Mode::RESULTS;
        search_of_ = q;
        current_url_ = "nuby://search?q=" + url_encode(q);
        input_url_ = current_url_;
        current_title_ = q + " - Buscar";

        auto t0 = std::chrono::steady_clock::now();
        auto hits = index_sp_->query(q, 24);
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        status_ = std::to_string(hits.size()) + " resultados";

        const int pad = (W >= 720) ? 60 : 18;
        std::ostringstream h;
        h << "<div style=\"padding: 18px " << pad << "px 40px " << pad << "px;\">"
          // fila superior: mini-logo + caja con la query (como todo buscador)
             "<div style=\"display: flex; flex-direction: row; align-items: center; gap: 14px; margin-bottom: 10px;\">"
               "<a href=\"nuby://home\" style=\"color: #1a73e8; font-size: 20px; font-weight: 800;\">Nuby</a>"
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
        render_content_doc(h.str(), "", {});
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
        render_content_doc(h.str(), "", {});
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
        render_content_doc(h.str(), "", {});
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
             "<span data-action=\"clear-history\" style=\"color: #b3261e; font-size: 13px; font-weight: 700;\">vaciar historial</span>"
             "<span style=\"color: #999;\"> · </span>"
             "<a href=\"nuby://home\" style=\"color: #0b57d0; font-size: 13px;\">inicio</a></div>"
          "</div>";
        render_content_doc(h.str(), "", {});
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
