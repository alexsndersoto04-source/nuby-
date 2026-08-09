#pragma once

#include "layout_box.hpp"
#include "flex_layout.hpp"
#include "text_shaper.hpp"
#include "../css/cascade.hpp"
#include "../html/document.hpp"
#include "../core/types.hpp"
#include <memory>
#include <vector>

namespace nuby::layout {

class LayoutEngine {
private:
    css::CascadeEngine cascade_;
    core::RectF viewport_{0, 0, 1000, 800};

    std::shared_ptr<LayoutBox> build_layout_tree(const std::shared_ptr<html::Node>& node, const css::ComputedStyle* parent_style = nullptr) {
        if (!node) return nullptr;

        if (node->is_element()) {
            auto elem = std::static_pointer_cast<html::Element>(node);
            css::ComputedStyle style = cascade_.compute_style(elem, parent_style);

            if (style.display == css::Display::NONE) {
                return nullptr; // display: none elements are not in the layout tree!
            }

            BoxType type = BoxType::BLOCK_BOX;
            if (style.display == css::Display::FLEX) {
                type = BoxType::FLEX_CONTAINER_BOX;
            } else if (style.display == css::Display::INLINE) {
                type = BoxType::INLINE_BOX;
            } else if (style.display == css::Display::INLINE_BLOCK) {
                type = BoxType::INLINE_BLOCK_BOX;
            }

            auto box = std::make_shared<LayoutBox>(type);
            box->style = style;
            box->node = node;

            for (const auto& child_node : node->get_children()) {
                auto child_box = build_layout_tree(child_node, &style);
                if (child_box) {
                    box->append_child(child_box);
                }
            }

            return box;
        } else if (node->is_text()) {
            auto text_node = std::static_pointer_cast<html::TextNode>(node);
            std::string text = core::StringUtils::trim(text_node->get_text());
            if (text.empty()) return nullptr;

            auto box = std::make_shared<LayoutBox>(BoxType::TEXT_BOX);
            if (parent_style) {
                box->style = *parent_style;
            }
            box->node = node;
            return box;
        }

        return nullptr;
    }

    void layout_block_children(const std::shared_ptr<LayoutBox>& box, core::RectF& containing_block) {
        float cursor_y = box->dimensions.content.y;
        float prev_margin_bottom = 0.0f;

        for (auto& child : box->children) {
            if (child->style.position == css::Position::ABSOLUTE || child->style.position == css::Position::FIXED) {
                continue; // Positioned out of normal flow
            }

            if (child->box_type == BoxType::TEXT_BOX) {
                // Layout text inside block
                auto text_node = std::static_pointer_cast<html::TextNode>(child->node);
                float max_w = box->dimensions.content.width;
                child->text_runs = TextShaper::wrap_text(
                    text_node->get_text(), max_w,
                    child->style.font_size, child->style.font_weight,
                    child->style.color, child->style.line_height
                );

                float text_height = 0.0f;
                for (auto& run : child->text_runs) {
                    run.rect.x += box->dimensions.content.x;
                    run.rect.y += cursor_y;
                    text_height += run.rect.height;
                }

                child->dimensions.content.x = box->dimensions.content.x;
                child->dimensions.content.y = cursor_y;
                child->dimensions.content.width = max_w;
                child->dimensions.content.height = text_height > 0 ? text_height : child->style.line_height;

                cursor_y += child->dimensions.content.height;
            } else if (child->box_type == BoxType::FLEX_CONTAINER_BOX) {
                // Algoritmo real de 3 pasos (como hacen los motores):
                layout_box_geometry(child, containing_block, cursor_y, prev_margin_bottom);

                // PASO 1 — medida: pre-maqueta el contenido de cada item para
                // conocer su tamaño intrínseco (los textos mandan)
                for (auto& item : child->children) {
                    layout_box_geometry(item, child->dimensions.content,
                                        child->dimensions.content.y, 0.0f);
                    core::RectF item_cb = item->dimensions.content;
                    layout_block_children(item, item_cb);
                }

                // PASO 2 — el algoritmo flex decide tamaños y posiciones finales
                FlexLayoutEngine::layout_flex_container(child, containing_block);

                // PASO 3 — re-maqueta CADA item completo con su rect final
                // (segunda pasada real; nada de translates con deltas)
                for (auto& item : child->children) {
                    core::RectF item_cb = item->dimensions.content;
                    layout_block_children(item, item_cb);
                    if (item->style.height.is_auto()) {
                        float content_h = 0.0f;
                        for (auto& grand : item->children)
                            content_h = std::max(content_h, grand->dimensions.margin_box().bottom()
                                                              - item->dimensions.content.y);
                        float pad_b = item->dimensions.padding.bottom + item->dimensions.border.bottom;
                        item->dimensions.content.height = std::max(item->dimensions.content.height,
                                                                   content_h);
                        (void)pad_b;
                    }
                }

                cursor_y = child->dimensions.margin_box().bottom();
                prev_margin_bottom = child->dimensions.margin.bottom;
            } else {
                // Block level layout
                layout_box_geometry(child, containing_block, cursor_y, prev_margin_bottom);
                
                core::RectF child_cb = child->dimensions.content;
                layout_block_children(child, child_cb);

                // If height is auto, compute from children
                if (child->style.height.is_auto()) {
                    float children_h = 0.0f;
                    for (const auto& grand : child->children) {
                        children_h = std::max(children_h, grand->dimensions.margin_box().bottom() - child->dimensions.content.y);
                    }
                    child->dimensions.content.height = std::max(children_h, child->style.min_height.resolve(containing_block.height));
                }

                cursor_y = child->dimensions.margin_box().bottom();
                prev_margin_bottom = child->dimensions.margin.bottom;
            }
        }

        if (box->style.height.is_auto()) {
            box->dimensions.content.height = std::max(0.0f, cursor_y - box->dimensions.content.y);
        }
    }

    void layout_box_geometry(const std::shared_ptr<LayoutBox>& box, const core::RectF& containing_block, float cursor_y, float prev_margin_bottom) {
        auto& style = box->style;
        auto& dims = box->dimensions;

        // Resolve Paddings and Borders
        dims.padding.top = style.padding_top.resolve(containing_block.width);
        dims.padding.right = style.padding_right.resolve(containing_block.width);
        dims.padding.bottom = style.padding_bottom.resolve(containing_block.width);
        dims.padding.left = style.padding_left.resolve(containing_block.width);

        dims.border.top = style.border_top_width;
        dims.border.right = style.border_right_width;
        dims.border.bottom = style.border_bottom_width;
        dims.border.left = style.border_left_width;

        dims.margin.left = style.margin_left.resolve(containing_block.width);
        dims.margin.right = style.margin_right.resolve(containing_block.width);
        dims.margin.top = style.margin_top.resolve(containing_block.width);
        dims.margin.bottom = style.margin_bottom.resolve(containing_block.width);

        // Standard CSS Margin Collapsing
        float collapsed_margin_top = std::max(dims.margin.top, prev_margin_bottom) - prev_margin_bottom;

        // Resolve Width
        if (style.width.is_auto()) {
            float available_w = containing_block.width - dims.margin.horizontal() - dims.padding.horizontal() - dims.border.horizontal();
            dims.content.width = std::max(0.0f, available_w);
        } else {
            dims.content.width = style.width.resolve(containing_block.width);
        }

        // Center with margin: auto
        if (style.margin_left.is_auto() && style.margin_right.is_auto()) {
            float total_box_w = dims.content.width + dims.padding.horizontal() + dims.border.horizontal();
            float rem = std::max(0.0f, containing_block.width - total_box_w);
            dims.margin.left = dims.margin.right = rem / 2.0f;
        }

        // Resolve Height
        if (!style.height.is_auto()) {
            dims.content.height = style.height.resolve(containing_block.height);
        } else {
            dims.content.height = 0.0f;
        }

        // Set Position
        dims.content.x = containing_block.x + dims.margin.left + dims.border.left + dims.padding.left;
        dims.content.y = cursor_y + collapsed_margin_top + dims.border.top + dims.padding.top;
    }

    // Desplaza recursivamente un subárbol completo (cajas y text runs).
    // Así los motores reales reubican contenido tras posicionamiento flex.
    void translate_subtree(const std::shared_ptr<LayoutBox>& box, float dx, float dy) {
        if (!box) return;
        box->dimensions.content.x += dx;
        box->dimensions.content.y += dy;
        for (auto& run : box->text_runs) {
            run.rect.x += dx;
            run.rect.y += dy;
        }
        for (auto& child : box->children) translate_subtree(child, dx, dy);
    }

    void layout_positioned_elements(const std::shared_ptr<LayoutBox>& box, const core::RectF& viewport) {
        for (auto& child : box->children) {
            if (child->style.position == css::Position::ABSOLUTE || child->style.position == css::Position::FIXED) {
                core::RectF cb = (child->style.position == css::Position::FIXED) ? viewport : box->dimensions.content;

                float x = cb.x;
                float y = cb.y;

                if (!child->style.left.is_auto()) {
                    x = cb.x + child->style.left.resolve(cb.width);
                } else if (!child->style.right.is_auto()) {
                    x = cb.right() - child->style.right.resolve(cb.width) - child->dimensions.content.width;
                }

                if (!child->style.top.is_auto()) {
                    y = cb.y + child->style.top.resolve(cb.height);
                } else if (!child->style.bottom.is_auto()) {
                    y = cb.bottom() - child->style.bottom.resolve(cb.height) - child->dimensions.content.height;
                }

                child->dimensions.content.x = x;
                child->dimensions.content.y = y;
            }
            layout_positioned_elements(child, viewport);
        }
    }

public:
    explicit LayoutEngine(core::RectF viewport = {0, 0, 1000, 800})
        : viewport_(viewport) {}

    void set_viewport(float w, float h) {
        viewport_ = core::RectF(0, 0, w, h);
    }

    void add_stylesheet(const css::StyleSheet& sheet) {
        cascade_.add_stylesheet(sheet);
    }

    void clear_stylesheets() {
        cascade_.clear_stylesheets();
    }

    std::shared_ptr<LayoutBox> layout(const std::shared_ptr<html::Document>& doc) {
        if (!doc || !doc->get_root_element()) return nullptr;

        auto root_box = build_layout_tree(doc->get_root_element());
        if (!root_box) return nullptr;

        // Set Root viewport dimensions
        root_box->dimensions.content = viewport_;
        core::RectF cb = viewport_;

        layout_block_children(root_box, cb);
        layout_positioned_elements(root_box, viewport_);

        return root_box;
    }
};

} // namespace nuby::layout
