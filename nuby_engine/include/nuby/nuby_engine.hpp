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
#include <memory>
#include <string>
#include <vector>

namespace nuby {

struct RenderResult {
    std::shared_ptr<html::Document> document;
    std::shared_ptr<layout::LayoutBox> layout_tree;
    paint::DisplayList display_list;
    std::vector<uint32_t> pixels;
    int width{1000};
    int height{800};
    core::Profiler profiler;
    std::vector<std::string> js_logs;
};

class NubyBrowserEngine {
private:
    int viewport_width_{1000};
    int viewport_height_{800};
    layout::LayoutEngine layout_engine_;
    std::shared_ptr<html::Document> current_document_;
    std::shared_ptr<layout::LayoutBox> current_layout_tree_;
    std::shared_ptr<js::JSEngine> current_js_engine_;

    void generate_display_list(const std::shared_ptr<layout::LayoutBox>& box, paint::DisplayList& dl) {
        if (!box) return;

        // 1. Box Shadow (if active)
        if (box->style.box_shadow.is_active) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_BOX_SHADOW;
            cmd.rect = box->dimensions.border_box();
            cmd.shadow_offset = core::PointF(box->style.box_shadow.offset_x, box->style.box_shadow.offset_y);
            cmd.blur_radius = box->style.box_shadow.blur_radius;
            cmd.color = box->style.box_shadow.color;
            dl.add_command(cmd);
        }

        // 2. Background (Color or Gradient)
        if (box->style.background_gradient.is_active) {
            paint::DrawCommand cmd;
            cmd.type = paint::CommandType::DRAW_LINEAR_GRADIENT;
            cmd.rect = box->dimensions.border_box();
            cmd.gradient_angle = box->style.background_gradient.angle_deg;
            cmd.gradient_stops = box->style.background_gradient.stops;
            dl.add_command(cmd);
        } else if (!box->style.background_color.is_transparent()) {
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
        if (box->style.border_top_width > 0 || box->style.border_right_width > 0 ||
            box->style.border_bottom_width > 0 || box->style.border_left_width > 0) {
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
            std::string combined_css = custom_css;
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
            try {
                current_js_engine_->eval(js_code);
            } catch (const std::exception& e) {
                result.js_logs.push_back(std::string("[Nuby JS error] ") + e.what());
            }
            result.js_logs = current_js_engine_->get_console_logs();
        }

        // Stage 4: Layout Tree Construction, Flexbox & Flow Resolution
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
            paint::SoftwareRasterizer rasterizer(viewport_width_, viewport_height_, core::Color::white());
            rasterizer.execute_display_list(result.display_list);
            result.pixels = rasterizer.get_pixels();
        }

        return result;
    }

    std::shared_ptr<js::JSEngine> get_js_engine() const { return current_js_engine_; }
};

} // namespace nuby
