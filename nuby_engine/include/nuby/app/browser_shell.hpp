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
    static constexpr int W = 1024;          // ancho ventana virtual
    static constexpr int H = 640;           // alto ventana virtual
    static constexpr int CHROME_H = 92;     // alto de la barra superior
    static constexpr int CONTENT_CAP = 8000; // tope de alto de página (seguridad)

    BrowserShell() {
        engine_chrome_ = std::make_unique<NubyBrowserEngine>(W, CHROME_H);
        engine_content_ = std::make_unique<NubyBrowserEngine>(W, CONTENT_CAP);
        go_home();
    }

    void set_data_path(const std::string& pages_tsv) { pages_path_ = pages_tsv; }
    search::SearchIndex& index() { return index_; }

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
        std::string& field = (focus_ == Focus::URL) ? input_url_ : input_search_;
        if (select_all_) { field.clear(); select_all_ = false; } // comportamiento real de url-bar
        field += utf8_encode(codepoint);
        if (focus_ == Focus::SEARCH && mode_ == Mode::HOME) render_home();
        rebuild_chrome();
        return true;
    }

    bool handle_key(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (key == "Backspace") {
            if (focus_ == Focus::NONE) return false;
            std::string& field = (focus_ == Focus::URL) ? input_url_ : input_search_;
            if (select_all_) { field.clear(); select_all_ = false; }
            if (field.empty()) return false;
            pop_utf8(field);
            if (focus_ == Focus::SEARCH && mode_ == Mode::HOME) render_home();
            rebuild_chrome();
            return true;
        }
        if (key == "Enter") {
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
                show_search_results(q);
                return true;
            }
            return false;
        }
        if (key == "Escape") {
            if (focus_ == Focus::NONE) return false;
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
        return index_.query(q, 24);
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
    enum class Focus { NONE, URL, SEARCH };
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

    search::SearchIndex index_;
    std::string pages_path_;

    std::shared_ptr<RenderResult> content_result_; // para hit-testing
    std::shared_ptr<js::Interpreter> js_;          // intérprete de la página actual
    std::string page_css_;                          // CSS de la página web actual (para re-renders)
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
    std::string chrome_html() const {
        bool url_focused = (focus_ == Focus::URL);
        std::string shown = url_focused ? input_url_ : current_url_;
        // scroll horizontal honesto: se ven los últimos caracteres
        const size_t MAX_VIS = 86;
        if (shown.size() > MAX_VIS) shown = "…" + shown.substr(shown.size() - MAX_VIS);

        std::string caret;
        if (url_focused && caret_on_)
            caret = "<div style=\"width: 2px; height: 22px; background-color: #111111; margin-left: 1px;\"></div>";

        std::string lock;
        if (current_url_.rfind("https://", 0) == 0)
            lock = "<span style=\"color: #188038; font-size: 13px; font-weight: 700;\">TLS·</span>";
        else if (current_url_.rfind("http://", 0) == 0)
            lock = "<span style=\"color: #b45309; font-size: 13px; font-weight: 700;\">http·</span>";

        std::string ring = url_focused ? "#0b57d0" : "#c7c7c7";
        std::string title_right = esc(current_title_);
        if (title_right.size() > 34) title_right = title_right.substr(0, 33) + "…";

        std::ostringstream h;
        h << "<div style=\"background-color: #f2f2f2; padding: 8px 10px 6px 10px;\">"

             "<div style=\"display: flex; flex-direction: row; align-items: center; gap: 8px;\">"
               "<div data-action=\"home\" style=\"background-color: #111111; padding: 6px 12px; border-radius: 8px;\">"
                 "<span style=\"color: #ffffff; font-size: 15px; font-weight: 800;\">NUBY</span></div>"
               "<div data-action=\"back\" style=\"background-color: #e4e4e4; padding: 6px 11px; border-radius: 8px;\">"
                 "<span style=\"color: #111111; font-size: 15px; font-weight: 700;\">&lt;</span></div>"
               "<div data-action=\"fwd\" style=\"background-color: #e4e4e4; padding: 6px 11px; border-radius: 8px;\">"
                 "<span style=\"color: #111111; font-size: 15px; font-weight: 700;\">&gt;</span></div>"
               "<div data-action=\"reload\" style=\"background-color: #e4e4e4; padding: 6px 11px; border-radius: 8px;\">"
                 "<span style=\"color: #111111; font-size: 15px; font-weight: 700;\">R</span></div>"
               "<div data-action=\"focus-url\" style=\"flex-grow: 1; background-color: #ffffff; border: 2px solid " << ring << "; border-radius: 10px; padding: 7px 10px;\">"
                 "<div style=\"display: flex; flex-direction: row; align-items: center;\">" << lock
                 << "<span style=\"color: #202020; font-size: 15px;\">" << esc(shown) << "</span>" << caret
                 << "</div></div>"
             "</div>"

             "<div style=\"display: flex; flex-direction: row; justify-content: space-between; margin-top: 7px;\">"
               "<span style=\"color: #555555; font-size: 12px;\">" << esc(status_) << "</span>"
               "<div data-action=\"about\" style=\"padding: 0px 4px;\">"
                 "<span style=\"color: #0b57d0; font-size: 12px; font-weight: 700;\">nuby://about · motor propio, cero Chrome</span></div>"
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
                if (el->has_attribute("data-action") || el->has_attribute("data-nuby-input") ||
                    (el->get_tag_name() == "a" && el->has_attribute("href")) ||
                    el->has_attribute("onclick"))
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
        rebuild_chrome();
        if (!el) return false;

        // Input propio de Nuby (caja de búsqueda en la home)
        if (el->has_attribute("data-nuby-input")) {
            focus_ = Focus::SEARCH;
            input_search_.clear();
            status_ = "Escribe tu busqueda y pulsa Enter — indice local BM25 real";
            render_home();
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
            auto res = std::make_shared<RenderResult>(
                engine_content_->render_page(serialize_body(content_result_->document),
                                             page_css_.empty() ? UA_EXTRA_CSS : page_css_));
            content_result_ = res;
            content_fb_ = res->pixels;
            // El re-render creó un documento NUEVO: el intérprete viejo quedaba
            // apuntando a un documento muerto y los siguientes clicks caían en un
            // fantasma. Re-bind al documento vivo. Limitación honesta: aquí el
            // ESTADO JS (funciones/variables de la carga inicial) se reinicia.
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

        // --- Web real ---
        mode_ = Mode::WEB;
        current_url_ = url;
        input_url_ = url;
        status_ = "Descargando " + url + " …";
        scroll_y_ = 0;
        rebuild_chrome();

        auto t0 = now_ms();
        auto res = net::Fetcher::fetch(url);
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

        // Indexación incremental REAL: esta visita alimenta el buscador
        if (parse_res->document && parse_res->document->get_body()) {
            std::string text = parse_res->document->get_body()->get_text_content();
            if (text.size() > 40) {
                index_.add_document(current_url_, current_title_, text);
                if (!pages_path_.empty()) index_.save(pages_path_);
            }
        }

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
        status_ = "Listo — indice local: " + std::to_string(index_.document_count()) +
                  " docs / " + std::to_string(index_.term_count()) + " terminos (rastreo real)";
        render_home();
        rebuild_chrome();
    }

    void render_home() {
        std::ostringstream h;
        h << "<div style=\"padding: 48px 60px;\">"
             "<div style=\"text-align: center; margin-bottom: 8px;\">"
               "<h1 style=\"color: #111111; font-size: 54px; font-weight: 800; margin: 0;\">NUBY</h1>"
             "</div>"
             "<div style=\"text-align: center; margin-bottom: 26px;\">"
               "<span style=\"color: #666666; font-size: 14px;\">cada pixel de esta pagina lo rasteriza el motor C++, no tu navegador</span>"
             "</div>"
             "<div data-nuby-input=\"search\" style=\"border: 2px solid "
          << std::string(focus_ == Focus::SEARCH ? "#0b57d0" : "#bbb") << "; border-radius: 24px; padding: 13px 20px; margin: 0 auto; max-width: 620px;\">"
               "<div style=\"display: flex; flex-direction: row; align-items: center;\">"
               "<span style=\"color: #888888; font-size: 15px; font-weight: 700;\">Q&nbsp; </span>"
               "<span style=\"color: " << (input_search_.empty() && focus_ != Focus::SEARCH ? "#9a9a9a" : "#111111")
          << "; font-size: 16px;\">"
          << esc(focus_ == Focus::SEARCH ? input_search_ : (input_search_.empty() ? "Busca en el indice local (rastreo real)…" : input_search_))
          << "</span>";
        if (focus_ == Focus::SEARCH && caret_on_)
            h << "<div style=\"width: 2px; height: 20px; background-color: #111111; margin-left: 2px;\"></div>";
        h << "</div></div>"
             "<div style=\"text-align: center; margin-top: 26px;\">"
               "<span style=\"color: #444444; font-size: 13px;\">Escribe una URL arriba, o busca en el indice: "
             << std::to_string(index_.document_count()) << " paginas reales rastreadas · ranking BM25</span>"
             "</div>"
             "<div style=\"text-align: center; margin-top: 14px;\">"
               "<a href=\"nuby://about\" style=\"color: #0b57d0; font-size: 13px;\">que es real y que no en este navegador</a>"
               "<span style=\"color: #999999; font-size: 13px;\"> · </span>"
               "<a href=\"nuby://history\" style=\"color: #0b57d0; font-size: 13px;\">historial</a>"
             "</div>"
          "</div>";
        render_content_doc(h.str(), "", {});
    }

    void show_search_results(const std::string& q) {
        mode_ = Mode::RESULTS;
        search_of_ = q;
        current_url_ = "nuby://search?q=" + url_encode(q);
        input_url_ = current_url_;
        current_title_ = "Buscar: " + q;

        status_ = "Buscando en indice BM25…";
        rebuild_chrome();
        auto t0 = now_ms();
        auto hits = index_.query(q, 24);
        long ms = now_ms() - t0;
        status_ = std::to_string(hits.size()) + " resultados en " + std::to_string(ms) +
                  " ms · BM25 sobre " + std::to_string(index_.document_count()) + " docs reales";

        std::ostringstream h;
        h << "<div style=\"padding: 26px 60px;\">"
             "<div style=\"margin-bottom: 4px;\">"
               "<a href=\"nuby://home\" style=\"color: #111111; font-size: 22px; font-weight: 800;\">NUBY</a>"
             "</div>"
             "<p style=\"color: #555555; font-size: 13px; margin-bottom: 18px;\">Resultados para «" << esc(q)
          << "» — puntuados con BM25 de verdad, nada prefabricado</p>";

        if (hits.empty()) {
            h << "<p style=\"color: #333333; font-size: 15px;\">Sin resultados en el indice local. "
                 "Navega a sitios reales y cada visita se indexa automaticamente.</p>";
        }
        for (const auto& hit : hits) {
            h << "<div style=\"margin-bottom: 18px;\">"
                 "<div style=\"margin-bottom: 1px;\"><span style=\"color: #0f7a34; font-size: 12px;\">"
              << esc(hit.doc->domain) << " · score " << score_str(hit.score) << "</span></div>"
                 "<a href=\"" << esc(hit.doc->url) << "\" style=\"color: #0b57d0; font-size: 17px; font-weight: 700;\">"
              << esc(hit.doc->title) << "</a>"
                 "<p style=\"color: #3c3c3c; font-size: 13px; margin-top: 2px;\">" << esc(hit.snippet) << "</p>"
              "</div>";
        }
        h << "</div>";
        render_content_doc(h.str(), "", {});
        rebuild_chrome();
    }

    static std::string score_str(double s) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.2f", s);
        return buf;
    }

    void show_error(const std::string& url, const std::string& err) {
        mode_ = Mode::ERROR_PAGE;
        current_title_ = "Error";
        status_ = "Fallo de navegacion";
        std::ostringstream h;
        h << "<div style=\"padding: 60px 70px;\">"
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
        current_url_ = "nuby://about";
        input_url_ = "nuby://about";
        current_title_ = "Nuby — la verdad tecnica";
        status_ = "nuby://about";
        std::ostringstream h;
        h << "<div style=\"padding: 34px 60px;\">"
             "<h1 style=\"color: #111111; font-size: 28px; margin: 0 0 12px 0;\">Que es real aqui (y que no)</h1>"
             "<p style=\"font-size: 14px; color: #333;\">Esta pagina, la barra de arriba y todo lo que ves "
             "lo calcularon el parser HTML, la cascada CSS, el motor de layout y el rasterizador de Nuby. "
             "Tu navegador solo muestra los pixeles, como un monitor.</p>"
             "<h2 style=\"font-size: 19px; margin: 18px 0 6px 0;\">REAL en esta build</h2>"
             "<p style=\"font-size: 13px; color: #222;\">"
             "· Parser HTML, CSS (cascada + especificidad), layout de bloques/flex, rasterizador propio AA<br>"
             "· Red: DNS+TCP+HTTP/1.1 propios; HTTPS via TLS real del OpenSSL del sistema; dechunking real<br>"
             "· Buscador: indice invertido + BM25 propios (" << std::to_string(index_.document_count())
          << " docs, " << std::to_string(index_.term_count()) << " terminos, rastreo real del 8-ago-2026)<br>"
             "· JavaScript: interprete real (lexer, parser, AST, closures) para un subconjunto documentado<br>"
             "· Historial, atras/adelante, e indexacion incremental al navegar: todo en memoria real</p>"
             "<h2 style=\"font-size: 19px; margin: 18px 0 6px 0;\">NO soportado todavia (la verdad)</h2>"
             "<p style=\"font-size: 13px; color: #222;\">"
             "· Imagenes (sin decodificadores JPEG/PNG enlazados): se muestran placeholders<br>"
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
        std::ostringstream h;
        h << "<div style=\"padding: 30px 60px;\">"
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
    )CSS";

    // frame compuesto
    std::shared_ptr<RenderResult> chrome_result_;
    std::vector<uint32_t> chrome_fb_;
    std::string content_base_url_;
};

} // namespace nuby::app
