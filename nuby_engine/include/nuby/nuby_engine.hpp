#pragma once

#include "core/types.hpp"
#include "core/profiler.hpp"
#include "html/parser.hpp"
#include "html/document.hpp"
#include "css/css_parser.hpp"
#include "css/cascade.hpp"
#include "layout/layout_engine.hpp"
#include "layout/layout_box.hpp"
#include "paint/display_list.hpp"
#include "paint/rasterizer.hpp"
#include "net/http_client.hpp"
#include "js/js_engine.hpp"
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace nuby {

// Hoja de estilo de agente de usuario DEL MOTOR (2026-08-09): como la de
// cualquier navegador, define la semántica de display del estándar HTML.
// Tiene la prioridad más baja: se antepone siempre y el CSS de la página
// puede sobreescribirla. Sin esto todo elemento nacía display:block y el
// inline flow (texto + enlaces en la misma línea) era imposible.
inline static const std::string ENGINE_UA_CSS = R"CSS(
    head, style, script, title, meta, link, base, template, noscript { display: none; }
    a, abbr, acronym, b, bdi, bdo, big, br, cite, code, data, dfn, em, i,
    kbd, label, mark, output, q, rt, ruby, s, samp, small, span, strike,
    strong, sub, sup, time, tt, u, var, wbr { display: inline; }
    img, input, select, textarea, button, iframe, video, audio, canvas,
    object, embed, meter, progress { display: inline-block; }
)CSS";

// Resultado de un re-pintado por ventana (sin re-layout ni scripts).
struct RasterWindow {
    std::vector<uint32_t> pixels;  // ancho = viewport*escala, alto = filas*escala
    int width{0};                  // px de dispositivo
    int height{0};
    int top_logical{0};            // primera fila LÓGICA del búfer
    int rows_logical{0};           // filas LÓGICAS que cubre el búfer
};

struct RenderResult {
    std::shared_ptr<html::Document> document;
    std::shared_ptr<layout::LayoutBox> layout_tree;
    paint::DisplayList display_list;
    std::vector<uint32_t> pixels;
    int raster_top_logical{0};   // fila lógica donde empieza el búfer
    int raster_rows_logical{0};  // filas lógicas que cubre (0 → todo el viewport)
    int width{1000};
    int height{800};
    core::Profiler profiler;
    std::vector<std::string> js_logs;
};

// ============================================================================
// Controles de formulario REALES (2026-08-09): valor/placeholder/caret/check.
// El valor vive en el atributo `value` del DOM (nuestro modelo de estado),
// el placeholder cuando está vacío, password con bullets UTF-8 reales, y el
// caret solo cuando el shell marcó data-nuby-caret="1" (foco real).
// ============================================================================
namespace detail {

static inline void render_form_control(const std::shared_ptr<layout::LayoutBox>& box,
                                       const std::shared_ptr<html::Element>& el,
                                       paint::DisplayList& dl) {
    const core::RectF c = box->dimensions.content;
    const std::string tag = el->get_tag_name();
    std::string type = core::StringUtils::to_lower(el->get_attribute("type"));
    bool is_check = tag == "input" && (type == "checkbox" || type == "radio");
    bool is_button = tag == "button" ||
        (tag == "input" && (type == "submit" || type == "button" || type == "reset"));
    bool is_textish = tag == "textarea" ||
        (tag == "input" && (type.empty() || type == "text" || type == "search" ||
                            type == "email" || type == "url" || type == "password" ||
                            type == "number" || type == "tel"));

    float fs = box->style.font_size > 0 ? box->style.font_size : 15.0f;
    float lh = box->style.line_height > 0 ? box->style.line_height : fs * 1.35f;

    if (is_check) {
        if (el->has_attribute("checked")) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::FILL_ROUNDED_RECT;
            cmd.rect = { c.x + 3.5f, c.y + 3.5f,
                         std::max(0.0f, c.width - 7.0f), std::max(0.0f, c.height - 7.0f) };
            if (cmd.rect.width <= 0 || cmd.rect.height <= 0) return;
            cmd.radius = core::BorderRadius(type == "radio" ? cmd.rect.width / 2.0f : 2.0f);
            cmd.color = core::Color(11, 87, 208);
            dl.add_command(cmd);
        }
        return;
    }

    if (is_button) {
        std::string label = el->get_attribute("value");
        if (label.empty()) label = type == "reset" ? "Restablecer" : "Enviar";
        paint::DrawCommand cmd;
        cmd.type = paint::CommandType::DRAW_TEXT;
        float tw = layout::TextShaper::measure_text_width(label, fs, 700);
        cmd.rect = { c.x + std::max(4.0f, (c.width - tw) / 2.0f),
                     c.y + std::max(1.0f, (c.height - fs * 1.25f) / 2.0f),
                     tw, fs * 1.25f };
        cmd.text = label;
        cmd.font_size = fs;
        cmd.font_weight = 700;
        cmd.color = box->style.color;
        dl.add_command(cmd);
        return;
    }

    if (!is_textish) return;

    {   // clip: el texto no se sale del campo (clip REAL del rasterizador)
        paint::DrawCommand clip;
        clip.type = paint::CommandType::PUSH_CLIP;
        clip.rect = c;
        dl.add_command(clip);
    }

    std::string val = el->get_attribute("value");
    core::Color color = box->style.color;
    if (type == "password" && !val.empty()) {
        size_t n = 0;
        for (unsigned char ch : val) if ((ch & 0xC0) != 0x80) ++n; // codepoints
        std::string dots;
        for (size_t i = 0; i < n; ++i) dots += "\xE2\x80\xA2"; // •
        val = dots;
    }
    bool placeholder = val.empty();
    if (placeholder) {
        val = el->get_attribute("placeholder");
        color = core::Color(138, 143, 152);
    }

    float y0 = c.y + (tag == "input" ? std::max(2.0f, (c.height - lh) / 2.0f) : 3.0f);
    float y = y0, last_w = 0.0f, last_y = y0;

    if (!val.empty()) {
        // líneas reales: partir por \n y envolver cada una
        std::vector<std::string> lines;
        std::string cur;
        for (char ch : val) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch;
        }
        lines.push_back(cur);
        for (auto& ln : lines) {
            auto runs = layout::TextShaper::wrap_text(ln, std::max(10.0f, c.width - 8.0f),
                                                      fs, box->style.font_weight, color, lh);
            if (runs.empty()) { y += lh; last_y = y - lh; last_w = 0; continue; }
            for (auto& r : runs) {
                paint::DrawCommand cmd;
                cmd.type = paint::CommandType::DRAW_TEXT;
                cmd.rect = { c.x + 4.0f, y, r.rect.width, r.rect.height };
                cmd.text = r.text;
                cmd.font_size = r.font_size;
                cmd.font_weight = r.font_weight;
                cmd.color = r.color;
                dl.add_command(cmd);
                last_y = y;
                last_w = r.rect.width;
                y += r.rect.height;
            }
        }
    }

    if (el->has_attribute("data-nuby-caret")) {
        paint::DrawCommand caret;
        caret.type = paint::CommandType::FILL_RECT;
        caret.rect = { c.x + 4.0f + (val.empty() ? 0.0f : last_w) + 1.0f,
                       (val.empty() ? y0 : last_y) + 2.0f, 2.0f, lh - 4.0f };
        caret.color = core::Color(11, 87, 208);
        dl.add_command(caret);
    }

    {
        paint::DrawCommand pop;
        pop.type = paint::CommandType::POP_CLIP;
        dl.add_command(pop);
    }
}

} // namespace detail

class NubyBrowserEngine {
private:
    int viewport_width_{1000};
    int viewport_height_{800};
    // ESCALA DE RENDER (2026-10-14): 1.0 = píxel lógico. En pantallas de alta
    // densidad (móviles, retina) el shell pide 2 o 3: el LAYOUT sigue
    // midiendo en px lógicos (todo el CSS y las decisiones responsive no
    // cambian) y el rasterizador pinta a esa resolución real. Es lo que
    // separa una interfaz nítida de una pixelada.
    float render_scale_{1.0f};
    layout::LayoutEngine layout_engine_;
    std::shared_ptr<html::Document> current_document_;

    std::shared_ptr<layout::LayoutBox> current_layout_tree_;
    // Alto REAL (px de dispositivo) que se rasterizó del último documento.
    // El shell lo lee para saber cuánto contenido tiene de verdad.
    int last_raster_height_{0};
    // ---- VENTANA de rasterizado ----
    // Se pinta solo la franja pedida (px lógicos). El layout NO se toca.
    int raster_top_{0};
    int raster_window_rows_{0};   // 0 → el viewport completo (render normal)
    int last_raster_top_{0};
    int last_raster_rows_{0};
    // RECORTE AL DOCUMENTO (2026-10-14): el búfer de una página no se reserva
    // al tope de 8000 filas, sino al alto REAL que midió el layout (+ margen).
    // En un móvil con densidad 3 eso es la diferencia entre reservar 112 MB
    // siempre y reservar ~15 MB en una página normal. El presupuesto de
    // píxeles es el techo honesto: si un documento es más alto que eso, se
    // rasteriza hasta ahí (límite documentado, no un fallo silencioso).
    bool trim_to_document_{false};
    size_t content_pixel_budget_{24u * 1000u * 1000u};
    std::shared_ptr<js::JSEngine> current_js_engine_;

    void generate_display_list(const std::shared_ptr<layout::LayoutBox>& box, paint::DisplayList& dl) {
        if (!box) return;

        // Una caja de TEXTO hereda el estilo completo del padre (color, fuente),
        // pero en CSS las decoraciones de caja (borde, fondo, sombra) NO se
        // pintan por cada fragmento de texto: las pinta la caja que las genera.
        // Sin este guard, cada línea de texto pintaba un borde/fondo duplicado.
        bool decorations = box->box_type != layout::BoxType::TEXT_BOX;

        // 1. Box Shadow (if active)
        if (decorations && box->style.box_shadow.is_active) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_BOX_SHADOW;
            cmd.rect = box->dimensions.border_box();
            cmd.shadow_offset = core::PointF(box->style.box_shadow.offset_x, box->style.box_shadow.offset_y);
            cmd.blur_radius = box->style.box_shadow.blur_radius;
            cmd.color = box->style.box_shadow.color;
            dl.add_command(cmd);
        }

        // 2. Background (Color or Gradient)
        if (decorations && box->style.background_gradient.is_active) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_LINEAR_GRADIENT;
            cmd.rect = box->dimensions.border_box();
            cmd.gradient_angle = box->style.background_gradient.angle_deg;
            cmd.gradient_stops = box->style.background_gradient.stops;
            dl.add_command(cmd);
        } else if (decorations && !box->style.background_color.is_transparent()) {
            paint::DrawCommand cmd;
            if (box->style.border_radius.has_radius()) {
                cmd.type = paint::CommandType::FILL_ROUNDED_RECT;
                cmd.rect = box->dimensions.border_box();
                cmd.radius = box->style.border_radius;
                cmd.color = box->style.background_color;
            } else {
                cmd.type = paint::CommandType::FILL_RECT;
                cmd.rect = box->dimensions.border_box();
                cmd.color = box->style.background_color;
            }
            dl.add_command(cmd);
        }

        // 3. Borders
        if (decorations &&
            (box->style.border_top_width > 0 || box->style.border_right_width > 0 ||
             box->style.border_bottom_width > 0 || box->style.border_left_width > 0)) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_BORDER;
            cmd.rect = box->dimensions.border_box();
            cmd.border_widths = core::Edges(
                box->style.border_top_width, box->style.border_right_width,
                box->style.border_bottom_width, box->style.border_left_width
            );
            cmd.border_color = box->style.border_color;
            dl.add_command(cmd);
        }

        // 3.5 Controles de formulario REALES (input/textarea/button)
        if (box->node && box->node->is_element()) {
            auto el = std::static_pointer_cast<html::Element>(box->node);
            const std::string& t = el->get_tag_name();
            if (t == "input" || t == "textarea" || t == "button")
                detail::render_form_control(box, el, dl);

            // 3.6 Imágenes REALES: decodificadas por el decodificador PNG propio
            if (t == "img") {
                if (el->decoded_image && el->decoded_image->valid()) {
                    paint::DrawCommand cmd;
                    cmd.type = paint::CommandType::DRAW_IMAGE;
                    cmd.rect = box->dimensions.content;
                    cmd.image = el->decoded_image;
                    dl.add_command(cmd);
                } else if (el->has_attribute("data-nuby-imgfail")) {
                    // placeholder HONESTO: dice por qué no se pudo mostrar
                    paint::DrawCommand box_cmd, txt;
                    box_cmd.type = paint::CommandType::FILL_RECT;
                    box_cmd.rect = box->dimensions.content;
                    box_cmd.color = core::Color(236, 239, 241);
                    dl.add_command(box_cmd);
                    std::string why = el->get_attribute("data-nuby-imgfail");
                    std::string alt = el->get_attribute("alt");
                    std::string label = "[img: " + (alt.empty() ? why : alt) + "]";
                    txt.type = paint::CommandType::DRAW_TEXT;
                    txt.rect = { box_cmd.rect.x + 4, box_cmd.rect.y + 4,
                                 box_cmd.rect.width - 8, 16.0f };
                    txt.text = label;
                    txt.font_size = 11.0f;
                    txt.font_weight = 400;
                    txt.color = core::Color(95, 99, 104);
                    dl.add_command(txt);
                }
            }
        }

        // 4. Text Content Runs
        for (const auto& run : box->text_runs) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_TEXT;
            cmd.rect = run.rect;
            cmd.text = run.text;
            cmd.font_size = run.font_size;
            cmd.font_weight = run.font_weight;
            cmd.color = run.color;
            dl.add_command(cmd);
        }

        // 5. Recursively render children
        for (const auto& child : box->children) {
            generate_display_list(child, dl);
        }
    }

public:
    NubyBrowserEngine(int width = 1000, int height = 800)
        : viewport_width_(width), viewport_height_(height), layout_engine_({0, 0, static_cast<float>(width), static_cast<float>(height)}) {}

    void set_viewport(int w, int h) {
        viewport_width_ = w;
        viewport_height_ = h;
        layout_engine_.set_viewport(static_cast<float>(w), static_cast<float>(h));
    }

    // Cambia la densidad de render (px de dispositivo por px lógico).
    // Debe ir seguido de un re-render (el shell lo hace en set_render_scale).
    void set_render_scale(float s) {
        if (s <= 0.01f) s = 1.0f;
        if (std::abs(s - render_scale_) < 0.001f) return;
        render_scale_ = s;
    }
    float render_scale() const { return render_scale_; }
    int pixel_width() const { return static_cast<int>(std::lround(viewport_width_ * render_scale_)); }
    int pixel_height() const { return static_cast<int>(std::lround(viewport_height_ * render_scale_)); }

    void set_trim_to_document(bool v) { trim_to_document_ = v; }
    // Pinta solo la franja [top, top+rows) en px lógicos. rows<=0 → viewport.
    void set_raster_window(int top, int rows) {
        raster_top_ = top < 0 ? 0 : top;
        raster_window_rows_ = rows <= 0 ? 0 : rows;
    }
    int last_raster_top() const { return last_raster_top_; }
    int last_raster_rows() const { return last_raster_rows_; }

    // Re-pinta el documento ACTUAL sin re-layout ni scripts (scroll por
    // ventana): el DOM, el layout y el intérprete quedan intactos.
    RasterWindow rasterize_window(int top_logical, int rows_logical) {
        set_raster_window(top_logical, rows_logical);
        RasterWindow out;
        if (!current_layout_tree_) return out;
        paint::DisplayList dl;
        generate_display_list(current_layout_tree_, dl);
        render_window_to(out, dl);
        return out;
    }

    void render_window_to(RasterWindow& out, const paint::DisplayList& dl) {
        last_raster_height_ = raster_rows_for(current_layout_tree_);
        last_raster_top_ = raster_top_;
        paint::SoftwareRasterizer rasterizer(viewport_width_, last_raster_height_,
                                             core::Color::white(), render_scale_);
        rasterizer.set_origin(0.0f, (float)raster_top_);
        rasterizer.execute_display_list(dl);
        out.pixels = rasterizer.get_pixels();
        out.width = rasterizer.get_width();
        out.height = rasterizer.get_height();
        out.top_logical = last_raster_top_;
        out.rows_logical = last_raster_height_;
    }
    void set_content_pixel_budget(size_t px) { content_pixel_budget_ = px; }
    int last_raster_height() const { return last_raster_height_; }

    static void walk_bottom_px(const std::shared_ptr<layout::LayoutBox>& box, float& bottom) {
        if (!box) return;
        bottom = std::max(bottom, box->dimensions.border_box().bottom());
        for (const auto& c : box->children) walk_bottom_px(c, bottom);
    }

    // Filas (px de dispositivo) que ocupará el rasterizado de este documento.
    int raster_rows_for(const std::shared_ptr<layout::LayoutBox>& tree) {
        int rows = raster_window_rows_ > 0 ? raster_window_rows_ : viewport_height_;
        if (trim_to_document_) {
            float bottom = 0.0f;
            walk_bottom_px(tree, bottom);
            const int doc_rows = (int)std::ceil(bottom) + 12;
            rows = std::min(rows, std::max(1, doc_rows - raster_top_));
        }
        // Techo de memoria: filas máximas que caben en el presupuesto.
        const int pw = std::max(1, (int)std::lround(viewport_width_ * render_scale_));
        const int max_rows = std::max(1, (int)(content_pixel_budget_ / (size_t)pw));
        if (rows > max_rows) rows = max_rows;
        last_raster_rows_ = rows;
        return rows;
    }

    RenderResult render_page(const std::string& html_code, const std::string& custom_css = "", const std::string& js_code = "") {
        RenderResult result;
        result.width = viewport_width_;
        result.height = viewport_height_;
        result.profiler.reset();

        // Stage 1: HTML Tokenization and Tree Construction (WHATWG Parser)
        {
            core::ScopedTimer timer(result.profiler, "HTML_Parsing", "Tokenize & build DOM tree");
            html::HTMLParser parser(html_code);
            current_document_ = parser.parse();
            result.document = current_document_;
        }

        // Stage 2: CSS Tokenization & Stylesheet Cascade
        {
            core::ScopedTimer timer(result.profiler, "CSS_Cascade", "Parse CSS & compute element styles");
            layout_engine_.clear_stylesheets();

            // Extract <style> elements from DOM
            std::string combined_css = ENGINE_UA_CSS + "\n" + custom_css;
            auto style_elems = current_document_->get_elements_by_tag_name("style");
            for (const auto& s_elem : style_elems) {
                combined_css += "\n" + s_elem->get_text_content();
            }

            if (!combined_css.empty()) {
                css::CSSParser css_parser(combined_css);
                auto sheet = css_parser.parse();
                layout_engine_.add_stylesheet(sheet);
            }
        }

        // Stage 3: JavaScript Execution & DOM Mutators
        current_js_engine_ = std::make_shared<js::JSEngine>(current_document_);
        if (!js_code.empty()) {
            core::ScopedTimer timer(result.profiler, "JS_Execution", "Real interpreter: lexer + AST + eval");
#if defined(__wasi__)
            // Build de navegador: no hay excepciones C++ (ver js/js_stub.hpp)
            current_js_engine_->eval(js_code);
#else
            try {
                current_js_engine_->eval(js_code);
            } catch (const std::exception& e) {
                result.js_logs.push_back(std::string("[Nuby JS error] ") + e.what());
            }
#endif
            // ANTES: `result.js_logs = get_console_logs();` BORRABA el error recién
            // anotado y nadie se enteraba del fallo. Ahora se conserva todo.
            auto logs = current_js_engine_->get_console_logs();
            result.js_logs.insert(result.js_logs.end(), logs.begin(), logs.end());
        }

        // Stage 3.5: Layout Tree Construction, Flexbox & Flow Resolution
        {
            core::ScopedTimer timer(result.profiler, "Layout_Engine", "BFC, IFC, Text Shaping & Flexbox");
            current_layout_tree_ = layout_engine_.layout(current_document_);
            result.layout_tree = current_layout_tree_;
        }

        // Stage 5: Display List Generation
        {
            core::ScopedTimer timer(result.profiler, "Display_List", "Generate 2D paint commands");
            generate_display_list(current_layout_tree_, result.display_list);
        }

        // Stage 6: 2D Software Rasterizer (Pixel Framebuffer Compositing)
        {
            core::ScopedTimer timer(result.profiler, "Rasterization", "Subpixel anti-aliasing & Porter-Duff blend");
            RasterWindow win;
            render_window_to(win, result.display_list);
            result.pixels = std::move(win.pixels);
            result.width = win.width;     // px de dispositivo
            result.height = win.height;
            result.raster_top_logical = win.top_logical;
            result.raster_rows_logical = win.rows_logical;
        }

        return result;
    }

    // Re-render de un documento YA VIVO (2026-08-09). ANTES, refrescar la
    // página tras editar un <input> se hacía serializando el DOM y
    // re-parseándolo: el documento NUEVO dejaba el foco apuntando a un
    // elemento muerto y el primer carácter tecleado se perdía en el vacío
    // (bug real detectado en la prueba de formularios en vivo).
    // Ahora: MISMO documento, misma identidad de nodos → foco y eventos
    // sobreviven a cualquier número de re-renders. No re-ejecuta los scripts
    // (mutarían dos veces); conserva el intérprete si ya está ligado a ESTE
    // documento (sus handlers JS siguen vivos).
    RenderResult render_document(const std::shared_ptr<html::Document>& doc,
                                 const std::string& custom_css) {
        RenderResult result;
        result.width = viewport_width_;
        result.height = viewport_height_;
        result.profiler.reset();

        current_document_ = doc;
        result.document = doc;

        // Stage 2: CSS (misma lógica que render_page)
        {
            core::ScopedTimer timer(result.profiler, "CSS_Cascade", "Parse CSS & compute element styles");
            layout_engine_.clear_stylesheets();
            std::string combined_css = ENGINE_UA_CSS + "\n" + custom_css;
            auto style_elems = current_document_->get_elements_by_tag_name("style");
            for (const auto& s_elem : style_elems) {
                combined_css += "\n" + s_elem->get_text_content();
            }
            if (!combined_css.empty()) {
                css::CSSParser css_parser(combined_css);
                auto sheet = css_parser.parse();
                layout_engine_.add_stylesheet(sheet);
            }
        }

        // Stage 3: JS — conserva el intérprete ligado a ESTE documento
        if (!current_js_engine_ || !current_js_engine_->interpreter() ||
            current_js_engine_->interpreter()->document().get() != doc.get()) {
            current_js_engine_ = std::make_shared<js::JSEngine>(current_document_);
        }

        // Stage 4: Layout
        {
            core::ScopedTimer timer(result.profiler, "Layout_Engine", "BFC, IFC, Text Shaping & Flexbox");
            current_layout_tree_ = layout_engine_.layout(current_document_);
            result.layout_tree = current_layout_tree_;
        }

        // Stage 5: Display List
        {
            core::ScopedTimer timer(result.profiler, "Display_List", "Generate 2D paint commands");
            generate_display_list(current_layout_tree_, result.display_list);
        }

        // Stage 6: Raster
        {
            core::ScopedTimer timer(result.profiler, "Rasterization", "Software rasterizer");
            RasterWindow win;
            render_window_to(win, result.display_list);
            result.pixels = std::move(win.pixels);
            result.width = win.width;     // px de dispositivo
            result.height = win.height;
            result.raster_top_logical = win.top_logical;
            result.raster_rows_logical = win.rows_logical;
        }

        return result;
    }

    std::shared_ptr<js::JSEngine> get_js_engine() const { return current_js_engine_; }
};

} // namespace nuby
