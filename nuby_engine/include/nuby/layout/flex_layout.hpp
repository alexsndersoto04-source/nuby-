#pragma once

#include "layout_box.hpp"
#include <vector>
#include <numeric>
#include <map>
#include <algorithm>

namespace nuby::layout {

class FlexLayoutEngine {
public:
    // Mide el tamaño principal intrínseco de un item: suma el ancho real del
    // contenido (textos ya formaados + cajas con width explícito), recursivo.
    static float measure_main_content(const std::shared_ptr<LayoutBox>& box) {
        if (!box) return 0.0f;
        // Texto: el ancho de la línea más larga (medido real).
        // OJO (2026-08-09): desde el IFC cada TextRun es UNA PALABRA, no una
        // línea entera — hay que agrupar por línea (misma y) y medir de la
        // primera a la última palabra. Antes medía la palabra más larga y
        // los flex items con texto quedaban aplastados a ese ancho.
        float w = 0.0f;
        {
            std::map<float, std::pair<float, float>> lines; // y → (min_x, max_right)
            for (const auto& run : box->text_runs) {
                auto it = lines.find(run.rect.y);
                if (it == lines.end())
                    lines[run.rect.y] = { run.rect.x, run.rect.right() };
                else {
                    it->second.first  = std::min(it->second.first, run.rect.x);
                    it->second.second = std::max(it->second.second, run.rect.right());
                }
            }
            for (const auto& kv : lines) w = std::max(w, kv.second.second - kv.second.first);
        }
        // Cajas con ancho explícito
        if (!box->style.width.is_auto()) {
            w = std::max(w, box->dimensions.content.width);
        }
        // Hijos: suman en bloque vertical (columna), comparten línea si son inline
        float kids_block = 0.0f, kids_inline_line = 0.0f;
        for (const auto& c : box->children) {
            float cw = measure_main_content(c)
                     + c->dimensions.margin.horizontal()
                     + c->dimensions.padding.horizontal()
                     + c->dimensions.border.horizontal();
            if (c->box_type == BoxType::INLINE_BOX || c->box_type == BoxType::TEXT_BOX
                || c->box_type == BoxType::INLINE_BLOCK_BOX) {
                kids_inline_line += cw; // en una fila de texto suman en línea
            } else {
                kids_block = std::max(kids_block, cw);
            }
        }
        return w + std::max(kids_block, kids_inline_line);
    }

    static void layout_flex_container(const std::shared_ptr<LayoutBox>& container, const core::RectF& containing_block) {
        if (container->children.empty()) return;

        const auto& style = container->style;
        bool is_row = (style.flex_direction == css::FlexDirection::ROW ||
                       style.flex_direction == css::FlexDirection::ROW_REVERSE);
        bool is_reverse = (style.flex_direction == css::FlexDirection::ROW_REVERSE ||
                           style.flex_direction == css::FlexDirection::COLUMN_REVERSE);
        bool do_wrap = (style.flex_wrap == css::FlexWrap::WRAP || style.flex_wrap == css::FlexWrap::WRAP_REVERSE);

        float container_main_size = is_row ? container->dimensions.content.width : container->dimensions.content.height;
        float container_cross_size = is_row ? container->dimensions.content.height : container->dimensions.content.width;

        float gap = style.gap;

        // Flex-WRAP REAL (2026-08-11): si wrap está activo, agrupa items en líneas
        // Cada línea se maqueta independiente con su propio free space.
        // Es real, no simulado: antes todo iba en una sola línea y desbordaba.
        if (do_wrap && is_row) {
            // Agrupar en líneas por ancho
            struct Line { std::vector<size_t> idx; float total_hyp = 0; };
            std::vector<Line> lines;
            Line cur;
            float cur_w = 0;
            // Necesitamos info hipotética primero para cada item
            std::vector<float> hyps;
            hyps.reserve(container->children.size());
            for (auto& child : container->children) {
                float intrinsic_w = measure_main_content(child)
                              + child->dimensions.margin.horizontal()
                              + child->dimensions.padding.horizontal()
                              + child->dimensions.border.horizontal();
                float w = child->style.width.is_auto() ? (intrinsic_w > 1.0f ? intrinsic_w : 120.0f)
                          : child->style.width.resolve(container->dimensions.content.width);
                hyps.push_back(w);
            }
            for (size_t i=0;i<container->children.size();++i) {
                float w = hyps[i];
                float need = cur.idx.empty() ? w : w + gap;
                if (!cur.idx.empty() && cur_w + need > container_main_size + 0.5f) {
                    lines.push_back(cur);
                    cur = Line{};
                    cur_w = 0;
                    need = w;
                }
                cur.idx.push_back(i);
                cur.total_hyp += w;
                cur_w += need;
            }
            if (!cur.idx.empty()) lines.push_back(cur);

            // Maquetar cada línea
            float cross_cursor = 0;
            float max_cross = 0;
            float line_gap = gap;
            for (auto& line : lines) {
                float total_hyp = line.total_hyp;
                float total_gaps = line.idx.size()>1 ? gap*(line.idx.size()-1) : 0;
                float free = container_main_size - (total_hyp + total_gaps);
                // distribuir free por flex_grow/shrink dentro de la línea
                float line_grow=0, line_shrink=0;
                for (size_t idx: line.idx) { line_grow+=container->children[idx]->style.flex_grow; line_shrink+=container->children[idx]->style.flex_shrink; }
                std::vector<float> target_w(line.idx.size());
                for (size_t k=0;k<line.idx.size();++k) {
                    size_t idx=line.idx[k];
                    float hyp = hyps[idx];
                    float tw = hyp;
                    if (free>0 && line_grow>0) tw += (container->children[idx]->style.flex_grow/line_grow)*free;
                    else if (free<0 && line_shrink>0) tw = std::max(20.0f, hyp - (container->children[idx]->style.flex_shrink/line_shrink)*(-free));
                    target_w[k]=tw;
                }
                // justify dentro de la línea
                float total_actual = 0; for(float v: target_w) total_actual+=v;
                float rem = container_main_size - (total_actual + total_gaps);
                float main_pos=0, spacing=gap;
                if (rem>0) {
                    switch(style.justify_content){
                        case css::JustifyContent::FLEX_END: main_pos=rem; break;
                        case css::JustifyContent::CENTER: main_pos=rem/2; break;
                        case css::JustifyContent::SPACE_BETWEEN: if(line.idx.size()>1) spacing=gap+rem/(line.idx.size()-1); break;
                        case css::JustifyContent::SPACE_AROUND: if(!line.idx.empty()){ float u=rem/line.idx.size(); main_pos=u/2; spacing=gap+u; } break;
                        case css::JustifyContent::SPACE_EVENLY: if(!line.idx.empty()){ float u=rem/(line.idx.size()+1); main_pos=u; spacing=gap+u; } break;
                        default: break;
                    }
                }
                // altura de la línea = max cross de items en la línea
                float line_cross=0;
                for (size_t k=0;k<line.idx.size();++k) {
                    size_t idx=line.idx[k];
                    auto& child = container->children[idx];
                    float intrinsic_h = 0;
                    { float bottom = child->dimensions.content.y; for(auto& grand: child->children) bottom=std::max(bottom, grand->dimensions.margin_box().bottom()); intrinsic_h = bottom - child->dimensions.content.y + child->dimensions.padding.vertical() + child->dimensions.border.vertical(); }
                    float h = child->style.height.is_auto() ? (intrinsic_h>1?intrinsic_h:40) : child->style.height.resolve(container->dimensions.content.height);
                    line_cross = std::max(line_cross, h);
                }
                if (container->style.height.is_auto() && line_cross==0) line_cross=40;
                // posicionar items de la línea
                float cur_main = main_pos;
                for (size_t k=0;k<line.idx.size();++k) {
                    size_t idx=line.idx[k];
                    auto& child = container->children[idx];
                    float tw = target_w[k];
                    float th = 0;
                    { float intrinsic_h = 0; for(auto& grand: child->children) intrinsic_h=std::max(intrinsic_h, grand->dimensions.margin_box().bottom() - child->dimensions.content.y); th = child->style.height.is_auto()? (intrinsic_h>1?intrinsic_h: line_cross) : child->style.height.resolve(container->dimensions.content.height); if (style.align_items==css::AlignItems::STRETCH && child->style.height.is_auto()) th = line_cross; }
                    float cross_pos=0;
                    if (style.align_items==css::AlignItems::CENTER) cross_pos=(line_cross - th)/2;
                    else if (style.align_items==css::AlignItems::FLEX_END) cross_pos=line_cross - th;
                    child->dimensions.content.x = container->dimensions.content.x + cur_main;
                    child->dimensions.content.y = container->dimensions.content.y + cross_cursor + cross_pos;
                    child->dimensions.content.width = tw;
                    child->dimensions.content.height = th;
                    cur_main += tw + spacing;
                }
                cross_cursor += line_cross + line_gap;
                max_cross = std::max(max_cross, line_cross);
            }
            if (container->style.height.is_auto()) {
                if (!lines.empty()) cross_cursor -= line_gap; // quitar último gap
                container->dimensions.content.height = cross_cursor;
            }
            return;
        }

        // 1. Calculate hypothetical main size for each item
        struct FlexItemInfo {
            std::shared_ptr<LayoutBox> box;
            float hypothetical_main_size{0.0f};
            float flex_grow{0.0f};
            float flex_shrink{1.0f};
            float target_main_size{0.0f};
            float target_cross_size{0.0f};
            float main_pos{0.0f};
            float cross_pos{0.0f};
        };

        std::vector<FlexItemInfo> items;
        float total_hypothetical_main = 0.0f;
        float total_grow = 0.0f;
        float total_shrink = 0.0f;

        for (auto& child : container->children) {
            FlexItemInfo info;
            info.box = child;
            info.flex_grow = child->style.flex_grow;
            info.flex_shrink = child->style.flex_shrink;

            // Tamaño intrínseco REAL: en el eje principal se mide el CONTENIDO
            // (texto + cajas con ancho explícito), no el ancho del pre-layout
            // a pantalla completa. En el cruzado se mide la altura alcanzada.
            float intrinsic_w = measure_main_content(child)
                              + child->dimensions.margin.horizontal()
                              + child->dimensions.padding.horizontal()
                              + child->dimensions.border.horizontal();
            float content_bottom = child->dimensions.content.y;
            for (auto& grand : child->children) {
                content_bottom = std::max(content_bottom, grand->dimensions.margin_box().bottom());
            }
            float intrinsic_h = content_bottom - child->dimensions.content.y
                              + child->dimensions.padding.vertical() + child->dimensions.border.vertical();

            if (is_row) {
                float w = child->style.width.is_auto()
                          ? (intrinsic_w > 1.0f ? intrinsic_w : 120.0f)
                          : child->style.width.resolve(container->dimensions.content.width);
                info.hypothetical_main_size = w;
                float h = child->style.height.is_auto()
                          ? (intrinsic_h > 1.0f ? intrinsic_h : 40.0f)
                          : child->style.height.resolve(container->dimensions.content.height);
                info.target_cross_size = h;
            } else {
                float h = child->style.height.is_auto()
                          ? (intrinsic_h > 1.0f ? intrinsic_h : 40.0f)
                          : child->style.height.resolve(container->dimensions.content.height);
                info.hypothetical_main_size = h;
                float w = child->style.width.is_auto() ? container->dimensions.content.width : child->style.width.resolve(container->dimensions.content.width);
                info.target_cross_size = w;
            }

            total_hypothetical_main += info.hypothetical_main_size;
            total_grow += info.flex_grow;
            total_shrink += info.flex_shrink;
            items.push_back(info);
        }

        float total_gaps = (items.size() > 1) ? gap * (items.size() - 1) : 0.0f;
        float free_space = container_main_size - (total_hypothetical_main + total_gaps);

        // 2. Distribute free space
        for (auto& item : items) {
            if (free_space > 0 && total_grow > 0.0f) {
                float grow_share = (item.flex_grow / total_grow) * free_space;
                item.target_main_size = item.hypothetical_main_size + grow_share;
            } else if (free_space < 0 && total_shrink > 0.0f) {
                float shrink_share = (item.flex_shrink / total_shrink) * (-free_space);
                item.target_main_size = std::max(20.0f, item.hypothetical_main_size - shrink_share);
            } else {
                item.target_main_size = item.hypothetical_main_size;
            }
        }

        // 3. Justify Content (Main Axis Distribution)
        float current_main_pos = 0.0f;
        float item_spacing = gap;

        // ALTURA PROVISIONAL REAL: con height:auto el contenedor mide 0 al
        // entrar aquí, y align-items:center calculaba offsets negativos
        // (bug real). La especificación deriva la altura del contenido:
        if (is_row && container->style.height.is_auto()) {
            float provisional = 0.0f;
            for (const auto& it : items) provisional = std::max(provisional, it.target_cross_size);
            if (provisional > container_cross_size) container_cross_size = provisional;
        }

        float total_actual_main = 0.0f;
        for (const auto& item : items) {
            total_actual_main += item.target_main_size;
        }
        float remaining_space = container_main_size - (total_actual_main + total_gaps);

        if (remaining_space > 0) {
            switch (style.justify_content) {
                case css::JustifyContent::FLEX_END:
                    current_main_pos = remaining_space;
                    break;
                case css::JustifyContent::CENTER:
                    current_main_pos = remaining_space / 2.0f;
                    break;
                case css::JustifyContent::SPACE_BETWEEN:
                    if (items.size() > 1) {
                        item_spacing = gap + (remaining_space / (items.size() - 1));
                    }
                    break;
                case css::JustifyContent::SPACE_AROUND:
                    if (!items.empty()) {
                        float unit = remaining_space / items.size();
                        current_main_pos = unit / 2.0f;
                        item_spacing = gap + unit;
                    }
                    break;
                case css::JustifyContent::SPACE_EVENLY:
                    if (!items.empty()) {
                        float unit = remaining_space / (items.size() + 1);
                        current_main_pos = unit;
                        item_spacing = gap + unit;
                    }
                    break;
                case css::JustifyContent::FLEX_START:
                default:
                    current_main_pos = 0.0f;
                    break;
            }
        }

        // 4. Align Items (Cross Axis Distribution) & Position Items
        float max_cross = 0.0f;

        for (size_t i = 0; i < items.size(); ++i) {
            auto& item = items[i];
            item.main_pos = current_main_pos;
            current_main_pos += item.target_main_size + item_spacing;

            // Cross axis alignment
            switch (style.align_items) {
                case css::AlignItems::CENTER:
                    item.cross_pos = (container_cross_size - item.target_cross_size) / 2.0f;
                    break;
                case css::AlignItems::FLEX_END:
                    item.cross_pos = container_cross_size - item.target_cross_size;
                    break;
                case css::AlignItems::STRETCH:
                    if (container_cross_size > 0 && item.box->style.height.is_auto()) {
                        item.target_cross_size = container_cross_size;
                    }
                    item.cross_pos = 0.0f;
                    break;
                case css::AlignItems::FLEX_START:
                case css::AlignItems::BASELINE:
                default:
                    item.cross_pos = 0.0f;
                    break;
            }

            // Assign geometry to child box
            if (is_row) {
                item.box->dimensions.content.x = container->dimensions.content.x + item.main_pos;
                item.box->dimensions.content.y = container->dimensions.content.y + item.cross_pos;
                item.box->dimensions.content.width = item.target_main_size;
                item.box->dimensions.content.height = item.target_cross_size;
            } else {
                item.box->dimensions.content.x = container->dimensions.content.x + item.cross_pos;
                item.box->dimensions.content.y = container->dimensions.content.y + item.main_pos;
                item.box->dimensions.content.width = item.target_cross_size;
                item.box->dimensions.content.height = item.target_main_size;
            }

            max_cross = std::max(max_cross, item.target_cross_size);
        }

        // If container height was auto, wrap to items cross size
        if (!is_row && container->style.height.is_auto()) {
            container->dimensions.content.height = current_main_pos - item_spacing;
        } else if (is_row && container->style.height.is_auto()) {
            container->dimensions.content.height = max_cross;
        }
    }
};

} // namespace nuby::layout
