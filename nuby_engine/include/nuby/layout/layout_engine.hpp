#pragma once

#include "layout_box.hpp"
#include "flex_layout.hpp"
#include "text_shaper.hpp"
#include "../css/cascade.hpp"
#include "../html/document.hpp"
#include "../core/types.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <algorithm>

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
            const std::string& raw = text_node->get_text();
            std::string text = core::StringUtils::trim(raw);
            if (text.empty()) return nullptr;

            auto box = std::make_shared<LayoutBox>(BoxType::TEXT_BOX);
            if (parent_style) {
                box->style = *parent_style;
            }
            box->node = node;
            // Whitespace del fuente a cada lado: el IFC decide con esto si
            // hay espacio real al concatenar con sus vecinos.
            unsigned char f = (unsigned char)raw.front();
            unsigned char b = (unsigned char)raw.back();
            box->ws_before = std::isspace(f) != 0;
            box->ws_after  = std::isspace(b) != 0;
            return box;
        }

        return nullptr;
    }

    // ------------------------------------------------------------------
    // IFC — Inline Formatting Context real (2026-08-09)
    // ------------------------------------------------------------------
    // Los motores de verdad (Blink, Gecko, WebKit) no apilan los inline como
    // bloques: forman LINEAS. Texto, <b>, <a> y cajas atómicas (img, input…)
    // fluyen de izquierda a derecha y envuelven al llegar al borde.
    // Esto implementa exactamente eso: itemización → line breaking →
    // posicionado con alineación de baseline → text-align real.

    struct InlineItem {
        enum Kind { WORD, ATOMIC, BRK } kind{WORD};
        std::string text;                        // WORD
        float width{0}, height{0};               // medida del ítem
        float font_size{16.0f};
        int   font_weight{400};
        float line_height{18.0f};                // WORD
        core::Color color{0, 0, 0, 255};
        bool  space_before{false};               // ¿lleva espacio delante?
        std::shared_ptr<LayoutBox> owner_text;   // TEXT_BOX dueño (WORD)
        std::vector<std::shared_ptr<LayoutBox>> chain; // INLINE_BOX ancestros
        std::shared_ptr<LayoutBox> atomic;       // ATOMIC (ya maquetado)
    };

    // ¿Puede esta caja participar en un IFC? Un inline con hijos de bloque
    // (HTML patológico: <a><div>…) no — cae al camino de bloque, honesto.
    bool ifc_compatible(const std::shared_ptr<LayoutBox>& box) {
        if (box->box_type == BoxType::TEXT_BOX) return true;
        if (box->box_type == BoxType::INLINE_BLOCK_BOX) return true; // atómica
        if (box->box_type != BoxType::INLINE_BOX) return false;
        for (auto& c : box->children) {
            if (c->box_type == BoxType::BLOCK_BOX || c->box_type == BoxType::FLEX_CONTAINER_BOX ||
                c->box_type == BoxType::ANONYMOUS_BLOCK_BOX || c->box_type == BoxType::FLEX_ITEM_BOX)
                return false;
            if (!ifc_compatible(c)) return false;
        }
        return true;
    }

    static std::string tag_of(const std::shared_ptr<LayoutBox>& box) {
        if (box->node && box->node->is_element())
            return std::static_pointer_cast<html::Element>(box->node)->get_tag_name();
        return "";
    }

    // Convierte los hijos inline del grupo en una secuencia plana de ítems,
    // maquetando YA las cajas atómicas (inline-block) para medirlas.
    void itemize_inline(const std::shared_ptr<LayoutBox>& box,
                        std::vector<std::shared_ptr<LayoutBox>>& chain,
                        std::vector<InlineItem>& out,
                        bool& pending_space,
                        const core::RectF& parent_cb) {
        if (box->box_type == BoxType::TEXT_BOX) {
            auto tn = std::static_pointer_cast<html::TextNode>(box->node);
            box->text_runs.clear(); // idempotente en re-renders
            if (box->ws_before) pending_space = true;
            auto words = core::StringUtils::split_whitespace(tn->get_text());
            for (auto& w : words) {
                InlineItem it;
                it.kind = InlineItem::WORD;
                it.text = w;
                it.font_size = box->style.font_size;
                it.font_weight = box->style.font_weight;
                it.line_height = box->style.line_height;
                it.color = box->style.color;
                it.width = TextShaper::measure_text_width(w, it.font_size, it.font_weight);
                it.space_before = pending_space;
                it.owner_text = box;
                it.chain = chain;
                out.push_back(std::move(it));
                pending_space = true; // palabras del mismo nodo van separadas
            }
            pending_space = box->ws_after;
            return;
        }

        if (tag_of(box) == "br") { // salto de línea real
            InlineItem it; it.kind = InlineItem::BRK; it.chain = chain;
            out.push_back(std::move(it));
            pending_space = false;
            return;
        }

        if (box->box_type == BoxType::INLINE_BLOCK_BOX) {
            // Atómica: se maqueta entera y luego flota en la línea.
            layout_box_geometry(box, parent_cb, 0.0f, 0.0f);
            core::RectF cb = box->dimensions.content;
            layout_block_children(box, cb);
            InlineItem it;
            it.kind = InlineItem::ATOMIC;
            it.width = box->dimensions.margin_box().width;
            it.height = box->dimensions.margin_box().height;
            it.space_before = pending_space;
            it.atomic = box;
            it.chain = chain;
            out.push_back(std::move(it));
            pending_space = true; // el fuente casi siempre separa con ws
            return;
        }

        // INLINE_BOX: transparente para el flujo, pero agrupa estilo/rect.
        chain.push_back(box);
        for (auto& c : box->children)
            itemize_inline(c, chain, out, pending_space, parent_cb);
        chain.pop_back();
    }

    // Maqueta los hijos inline [i0, i1) de `box` como líneas con wrap real.
    // Devuelve la altura total consumida.
    float layout_inline_context(const std::shared_ptr<LayoutBox>& box,
                                size_t i0, size_t i1, float top_y) {
        std::vector<InlineItem> items;
        std::vector<std::shared_ptr<LayoutBox>> chain;
        bool pending_space = false;
        for (size_t k = i0; k < i1; ++k)
            itemize_inline(box->children[k], chain, items, pending_space,
                           box->dimensions.content);
        if (items.empty()) return 0.0f;

        const float x0 = box->dimensions.content.x;
        const float maxw = box->dimensions.content.width;

        // ---- 1) line breaking ----
        struct Line { std::vector<size_t> idx; float width{0}; int gaps{0}; };
        std::vector<Line> lines;
        Line cur;
        auto space_w = [&](size_t item_idx) -> float {
            // el espacio se mide con la fuente del ítem anterior si existe
            if (item_idx > 0) {
                auto& p = items[item_idx - 1];
                return TextShaper::estimate_char_width(' ', p.font_size, p.font_weight);
            }
            return TextShaper::estimate_char_width(' ', items[item_idx].font_size,
                                                   items[item_idx].font_weight);
        };

        for (size_t n = 0; n < items.size(); ++n) {
            auto& it = items[n];
            if (it.kind == InlineItem::BRK) {
                if (!cur.idx.empty()) lines.push_back(cur);
                else lines.push_back(Line{}); // línea en blanco honesta (<br><br>)
                cur = Line{};
                continue;
            }
            float sp = (it.space_before && !cur.idx.empty()) ? space_w(n) : 0.0f;
            // Tolerancia subpíxel de 0.5px: sin ella, una línea medida a un
            // ancho EXACTO (flex shrink-to-fit) envuelve por epsilon de
            // redondeo flotante ("thrash"). Los motores reales hacen lo mismo.
            if (!cur.idx.empty() && cur.width + sp + it.width > maxw + 0.5f && maxw > 50.0f) {
                lines.push_back(cur);          // wrap real
                cur = Line{};
                sp = 0.0f;                     // el espacio al inicio de línea se come (spec)
            }
            if (sp > 0.0f) cur.gaps++;
            cur.width += sp + it.width;
            cur.idx.push_back(n);
        }
        if (!cur.idx.empty() || lines.empty()) lines.push_back(cur);

        // ---- 2) métricas de línea (baseline real por ítem) ----
        //   texto:  ascenso ≈ 0.9·font_size (glifo 8×12 escalado), el resto
        //           de su line-height es descenso.
        //   atómica: vertical-align: baseline → borde inferior del margin
        //           box sobre la baseline (ascenso = alto margin, descenso 0).
        auto ascent_of = [&](const InlineItem& it) -> float {
            return it.kind == InlineItem::ATOMIC ? it.height : 0.9f * it.font_size;
        };
        auto descent_of = [&](const InlineItem& it) -> float {
            return it.kind == InlineItem::ATOMIC ? 0.0f
                 : std::max(it.line_height - 0.9f * it.font_size, 0.0f);
        };

        // ---- 3) posicionado ----
        std::unordered_map<LayoutBox*, core::RectF> unions;
        std::unordered_map<LayoutBox*, bool> has_u;
        auto grow = [&](LayoutBox* b, const core::RectF& r) {
            if (!b) return;
            if (!has_u[b]) { unions[b] = r; has_u[b] = true; }
            else {
                auto& u = unions[b];
                float nx = std::min(u.x, r.x), ny = std::min(u.y, r.y);
                float nr = std::max(u.right(), r.right()), nb = std::max(u.bottom(), r.bottom());
                u = {nx, ny, nr - nx, nb - ny};
            }
        };

        float y = top_y;
        const css::TextAlign align = box->style.text_align;
        for (size_t li = 0; li < lines.size(); ++li) {
            auto& L = lines[li];
            if (L.idx.empty()) { y += box->style.line_height; continue; }

            float baseline = 0.0f, max_desc = 0.0f;
            for (size_t n : L.idx) {
                baseline = std::max(baseline, ascent_of(items[n]));
                max_desc = std::max(max_desc, descent_of(items[n]));
            }
            float line_h = baseline + max_desc;

            // text-align REAL por línea (justify no estira la última, spec)
            float xoff = 0.0f, gap_extra = 0.0f;
            float slack = maxw - L.width;
            if (align == css::TextAlign::CENTER && slack > 0) xoff = slack / 2.0f;
            else if (align == css::TextAlign::RIGHT && slack > 0) xoff = slack;
            else if (align == css::TextAlign::JUSTIFY && slack > 0 &&
                     L.gaps > 0 && li + 1 < lines.size())
                gap_extra = slack / (float)L.gaps;

            float x = x0 + xoff;
            for (size_t n : L.idx) {
                auto& it = items[n];
                if (it.space_before && x > x0 + xoff) x += space_w(n) + gap_extra;
                if (it.kind == InlineItem::WORD) {
                    TextRun run;
                    run.text = it.text;
                    run.font_size = it.font_size;
                    run.font_weight = it.font_weight;
                    run.color = it.color;
                    run.rect = { x, y + baseline - 0.9f * it.font_size,
                                 it.width, it.line_height };
                    it.owner_text->text_runs.push_back(run);
                    grow(it.owner_text.get(), run.rect);
                    for (auto& anc : it.chain) grow(anc.get(), run.rect);
                } else { // ATOMIC
                    auto mb = it.atomic->dimensions.margin_box();
                    float target_cx = x + it.atomic->dimensions.margin.left
                                    + it.atomic->dimensions.border.left
                                    + it.atomic->dimensions.padding.left;
                    float target_cy = y + baseline - mb.height
                                    + it.atomic->dimensions.margin.top
                                    + it.atomic->dimensions.border.top
                                    + it.atomic->dimensions.padding.top;
                    translate_subtree(it.atomic,
                                      target_cx - it.atomic->dimensions.content.x,
                                      target_cy - it.atomic->dimensions.content.y);
                    grow(it.atomic.get(), it.atomic->dimensions.margin_box());
                    for (auto& anc : it.chain) grow(anc.get(), it.atomic->dimensions.margin_box());
                }
                x += it.width;
            }
            y += line_h;
        }

        // ---- 4) rects finales de cajas inline/texto (para hit-testing y bg) ----
        for (auto& kv : unions) {
            kv.first->dimensions.content = kv.second;
            // inline puro: padding/border/margin no afectan al flow (v1 honesta)
            if (kv.first->box_type != BoxType::INLINE_BLOCK_BOX) {
                kv.first->dimensions.margin = {0, 0, 0, 0};
                kv.first->dimensions.padding = {0, 0, 0, 0};
                kv.first->dimensions.border = {0, 0, 0, 0};
            }
        }
        return y - top_y;
    }

    void layout_block_children(const std::shared_ptr<LayoutBox>& box, core::RectF& containing_block) {
        float cursor_y = box->dimensions.content.y;
        float prev_margin_bottom = 0.0f;

        // Bucle por índices: los hijos inline CONSECUTIVOS se agrupan y se
        // maquetan juntos en un Inline Formatting Context (líneas reales),
        // no apilados como si fueran bloques.
        for (size_t ci = 0; ci < box->children.size(); ++ci) {
            auto& child = box->children[ci];
            if (child->style.position == css::Position::ABSOLUTE || child->style.position == css::Position::FIXED) {
                continue; // Positioned out of normal flow
            }

            if (child->is_inline_level() && ifc_compatible(child)) {
                // Fin del grupo: primer hijo no-inline compatible o posicionado
                size_t cj = ci + 1;
                while (cj < box->children.size()) {
                    auto& nxt = box->children[cj];
                    if (nxt->style.position == css::Position::ABSOLUTE ||
                        nxt->style.position == css::Position::FIXED) { ++cj; continue; }
                    if (!nxt->is_inline_level() || !ifc_compatible(nxt)) break;
                    ++cj;
                }
                cursor_y += layout_inline_context(box, ci, cj, cursor_y);
                prev_margin_bottom = 0.0f; // no hay colapso de márgenes a través de líneas
                ci = cj - 1;
                continue;
            }

            if (child->box_type == BoxType::FLEX_CONTAINER_BOX) {
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

                // If height is auto, compute from children — EXCEPTO <img>:
                // su altura ya salió del tamaño intrínseco de la imagen real
                bool is_img = child->node && child->node->is_element() &&
                    std::static_pointer_cast<html::Element>(child->node)->get_tag_name() == "img";
                if (child->style.height.is_auto() && !is_img) {
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

        // <img> tampoco se pisa aquí: su altura auto ya salió del tamaño
        // intrínseco real de la imagen (layout_box_geometry), y al no tener
        // hijos que maquetar cursor_y jamás avanzó → quedaría en 0.
        bool self_is_img = box->node && box->node->is_element() &&
            std::static_pointer_cast<html::Element>(box->node)->get_tag_name() == "img";
        if (box->style.height.is_auto() && !self_is_img) {
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

        // Tamaño intrínseco de imágenes REALES (2026-08-09):
        // prioridad CSS px → atributos width/height HTML → tamaño natural.
        // Con una sola dimensión dada se conserva la proporción.
        if (box->node && box->node->is_element()) {
            auto el = std::static_pointer_cast<html::Element>(box->node);
            if (el->get_tag_name() == "img") {
                float nat_w = 120.0f, nat_h = 60.0f; // caja honesta si no hay imagen
                if (el->decoded_image && el->decoded_image->valid()) {
                    nat_w = (float)el->decoded_image->width;
                    nat_h = (float)el->decoded_image->height;
                }
                float aw = 0, ah = 0;
                if (el->has_attribute("width")) aw = (float)atof(el->get_attribute("width").c_str());
                if (el->has_attribute("height")) ah = (float)atof(el->get_attribute("height").c_str());

                float avail = containing_block.width - dims.margin.horizontal()
                            - dims.padding.horizontal() - dims.border.horizontal();
                if (style.width.is_auto()) {
                    float w = aw > 0 ? aw : nat_w;
                    dims.content.width = std::min(w, std::max(0.0f, avail));
                }
                if (style.height.is_auto()) {
                    float h = ah > 0 ? ah : nat_h;
                    if (nat_w > 0 && dims.content.width > 0 && aw <= 0 && ah <= 0) {
                        // proporción real si el ancho se limitó
                        h = dims.content.width * nat_h / nat_w;
                    } else if (nat_w > 0 && aw > 0 && ah <= 0) {
                        h = dims.content.width * nat_h / nat_w;
                    }
                    dims.content.height = std::max(0.0f, h);
                }
            }
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
