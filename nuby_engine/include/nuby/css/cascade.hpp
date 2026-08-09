#pragma once

#include "css_rule.hpp"
#include "css_value.hpp"
#include "../html/element.hpp"
#include "../core/types.hpp"
#include "../core/string_utils.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace nuby::css {

struct ComputedStyle {
    Display display{Display::BLOCK};
    Position position{Position::STATIC};

    Length top{Length::auto_val()};
    Length right{Length::auto_val()};
    Length bottom{Length::auto_val()};
    Length left{Length::auto_val()};

    Length width{Length::auto_val()};
    Length height{Length::auto_val()};
    Length min_width{Length::px(0)};
    Length min_height{Length::px(0)};

    Length margin_top{Length::px(0)};
    Length margin_right{Length::px(0)};
    Length margin_bottom{Length::px(0)};
    Length margin_left{Length::px(0)};

    Length padding_top{Length::px(0)};
    Length padding_right{Length::px(0)};
    Length padding_bottom{Length::px(0)};
    Length padding_left{Length::px(0)};

    float border_top_width{0.0f};
    float border_right_width{0.0f};
    float border_bottom_width{0.0f};
    float border_left_width{0.0f};

    core::Color border_color{core::Color::transparent()};
    BorderStyle border_style{BorderStyle::NONE};
    core::BorderRadius border_radius{0.0f};

    core::Color color{core::Color::black()};
    core::Color background_color{core::Color::transparent()};

    float font_size{16.0f};
    int font_weight{400}; // 400 normal, 700 bold
    float line_height{20.0f};
    std::string font_family{"sans-serif"};
    TextAlign text_align{TextAlign::LEFT};

    BoxShadow box_shadow;
    LinearGradient background_gradient;

    // Flex properties
    FlexDirection flex_direction{FlexDirection::ROW};
    JustifyContent justify_content{JustifyContent::FLEX_START};
    AlignItems align_items{AlignItems::STRETCH};
    float flex_grow{0.0f};
    float flex_shrink{1.0f};
    Length flex_basis{Length::auto_val()};
    float gap{0.0f};

    int z_index{0};
    float opacity{1.0f};

    // Matched rules for DevTools inspector
    struct MatchedRuleInfo {
        std::string selector;
        Specificity specificity;
        std::string property;
        std::string value;
        bool overridden{false};
    };
    std::vector<MatchedRuleInfo> matched_rules;
};

class CascadeEngine {
private:
    std::vector<StyleSheet> stylesheets_;
    StyleSheet user_agent_sheet_;

    void setup_user_agent_stylesheet() {
        // User Agent Default Stylesheet (HTML5 Default Rendering)
        std::string ua_css = R"(
            html, body, div, span, applet, object, iframe,
            h1, h2, h3, h4, h5, h6, p, blockquote, pre,
            a, abbr, acronym, address, big, cite, code,
            del, dfn, em, img, ins, kbd, q, s, samp,
            small, strike, strong, sub, sup, tt, var,
            b, u, i, center,
            dl, dt, dd, ol, ul, li,
            fieldset, form, label, legend,
            table, caption, tbody, tfoot, thead, tr, th, td,
            article, aside, canvas, details, embed, 
            figure, figcaption, footer, header, hgroup, 
            menu, nav, output, ruby, section, summary,
            time, mark, audio, video {
                margin: 0;
                padding: 0;
                border: 0;
                font-size: 16px;
                color: #1a1a1a;
            }
            body {
                display: block;
                font-size: 16px;
                line-height: 24px;
                color: #24292f;
                background-color: #ffffff;
                margin: 8px;
            }
            div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, section, header, footer, nav, article {
                display: block;
            }
            span, a, strong, em, b, i {
                display: inline;
            }
            h1 {
                font-size: 32px;
                font-weight: 700;
                margin-top: 18px;
                margin-bottom: 18px;
                line-height: 38px;
            }
            h2 {
                font-size: 24px;
                font-weight: 700;
                margin-top: 14px;
                margin-bottom: 14px;
                line-height: 30px;
            }
            h3 {
                font-size: 20px;
                font-weight: 600;
                margin-top: 12px;
                margin-bottom: 12px;
                line-height: 26px;
            }
            p {
                margin-top: 8px;
                margin-bottom: 8px;
            }
            a {
                color: #0969da;
                text-decoration: underline;
            }
            strong, b {
                font-weight: 700;
            }
            em, i {
                font-style: italic;
            }
            button {
                display: inline-block;
                padding: 8px 16px;
                font-size: 14px;
                font-weight: 600;
                border: 1px solid #d0d7de;
                border-radius: 6px;
                background-color: #f6f8fa;
                color: #24292f;
            }
            input {
                display: inline-block;
                padding: 6px 12px;
                font-size: 14px;
                border: 1px solid #d0d7de;
                border-radius: 6px;
            }
        )";
        CSSParser parser(ua_css);
        user_agent_sheet_ = parser.parse();
    }

    bool matches_simple_selector(const SimpleSelector& sel, const std::shared_ptr<html::Element>& elem) {
        switch (sel.type) {
            case SimpleSelectorType::UNIVERSAL:
                return true;
            case SimpleSelectorType::TYPE:
                return elem->get_tag_name() == sel.value;
            case SimpleSelectorType::CLASS:
                return elem->has_class(sel.value);
            case SimpleSelectorType::ID:
                return elem->get_id() == sel.value;
            case SimpleSelectorType::ATTRIBUTE:
                return elem->has_attribute(sel.value);
            case SimpleSelectorType::PSEUDO_CLASS:
                return true; // Simplified pseudo matching
            default:
                return false;
        }
    }

    bool matches_compound_selector(const CompoundSelector& compound, const std::shared_ptr<html::Element>& elem) {
        for (const auto& s : compound.simple_selectors) {
            if (!matches_simple_selector(s, elem)) return false;
        }
        return true;
    }

    bool matches_complex_selector(const ComplexSelector& complex, const std::shared_ptr<html::Element>& elem) {
        if (complex.compound_selectors.empty()) return false;

        // Match from right to left (browsers evaluate right-to-left for performance!)
        int comp_idx = static_cast<int>(complex.compound_selectors.size()) - 1;
        if (!matches_compound_selector(complex.compound_selectors[comp_idx], elem)) {
            return false;
        }

        std::shared_ptr<html::Node> curr = elem;
        comp_idx--;

        while (comp_idx >= 0 && curr) {
            Combinator comb = complex.compound_selectors[comp_idx].combinator;
            if (comb == Combinator::CHILD) {
                curr = curr->get_parent();
                if (!curr || !curr->is_element()) return false;
                auto curr_elem = std::static_pointer_cast<html::Element>(curr);
                if (!matches_compound_selector(complex.compound_selectors[comp_idx], curr_elem)) {
                    return false;
                }
                comp_idx--;
            } else { // DESCENDANT
                bool found = false;
                while ((curr = curr->get_parent()) != nullptr) {
                    if (curr->is_element()) {
                        auto curr_elem = std::static_pointer_cast<html::Element>(curr);
                        if (matches_compound_selector(complex.compound_selectors[comp_idx], curr_elem)) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) return false;
                comp_idx--;
            }
        }

        return comp_idx < 0;
    }

    void apply_declaration(ComputedStyle& style, const std::string& prop, const std::string& val, float parent_font_size) {
        std::string p = core::StringUtils::to_lower(prop);
        std::string v = core::StringUtils::trim(val);

        if (p == "display") {
            if (v == "none") style.display = Display::NONE;
            else if (v == "inline") style.display = Display::INLINE;
            else if (v == "inline-block") style.display = Display::INLINE_BLOCK;
            else if (v == "flex") style.display = Display::FLEX;
            else style.display = Display::BLOCK;
        } else if (p == "position") {
            if (v == "relative") style.position = Position::RELATIVE;
            else if (v == "absolute") style.position = Position::ABSOLUTE;
            else if (v == "fixed") style.position = Position::FIXED;
            else style.position = Position::STATIC;
        } else if (p == "width") {
            style.width = Length::parse(v);
        } else if (p == "height") {
            style.height = Length::parse(v);
        } else if (p == "min-width") {
            style.min_width = Length::parse(v);
        } else if (p == "min-height") {
            style.min_height = Length::parse(v);
        } else if (p == "color") {
            style.color = core::Color::parse(v);
        } else if (p == "background-color" || p == "background") {
            if (v.find("linear-gradient") != std::string::npos) {
                style.background_gradient = LinearGradient::parse(v);
            } else {
                style.background_color = core::Color::parse(v);
            }
        } else if (p == "font-size") {
            style.font_size = Length::parse(v).resolve(100.0f, parent_font_size, 16.0f);
        } else if (p == "font-weight") {
            if (v == "bold") style.font_weight = 700;
            else if (v == "normal") style.font_weight = 400;
            else {
                try { style.font_weight = std::stoi(v); } catch (...) { style.font_weight = 400; }
            }
        } else if (p == "line-height") {
            style.line_height = Length::parse(v).resolve(100.0f, style.font_size, 16.0f);
        } else if (p == "font-family") {
            style.font_family = v;
        } else if (p == "text-align") {
            if (v == "center") style.text_align = TextAlign::CENTER;
            else if (v == "right") style.text_align = TextAlign::RIGHT;
            else if (v == "justify") style.text_align = TextAlign::JUSTIFY;
            else style.text_align = TextAlign::LEFT;
        } else if (p == "margin") {
            auto tokens = core::StringUtils::split_whitespace(v);
            if (tokens.size() == 1) {
                Length l = Length::parse(tokens[0]);
                style.margin_top = style.margin_right = style.margin_bottom = style.margin_left = l;
            } else if (tokens.size() == 2) {
                Length v_l = Length::parse(tokens[0]);
                Length h_l = Length::parse(tokens[1]);
                style.margin_top = style.margin_bottom = v_l;
                style.margin_right = style.margin_left = h_l;
            } else if (tokens.size() == 4) {
                style.margin_top = Length::parse(tokens[0]);
                style.margin_right = Length::parse(tokens[1]);
                style.margin_bottom = Length::parse(tokens[2]);
                style.margin_left = Length::parse(tokens[3]);
            }
        } else if (p == "margin-top") style.margin_top = Length::parse(v);
        else if (p == "margin-right") style.margin_right = Length::parse(v);
        else if (p == "margin-bottom") style.margin_bottom = Length::parse(v);
        else if (p == "margin-left") style.margin_left = Length::parse(v);
        else if (p == "padding") {
            auto tokens = core::StringUtils::split_whitespace(v);
            if (tokens.size() == 1) {
                Length l = Length::parse(tokens[0]);
                style.padding_top = style.padding_right = style.padding_bottom = style.padding_left = l;
            } else if (tokens.size() == 2) {
                Length v_l = Length::parse(tokens[0]);
                Length h_l = Length::parse(tokens[1]);
                style.padding_top = style.padding_bottom = v_l;
                style.padding_right = style.padding_left = h_l;
            } else if (tokens.size() == 4) {
                style.padding_top = Length::parse(tokens[0]);
                style.padding_right = Length::parse(tokens[1]);
                style.padding_bottom = Length::parse(tokens[2]);
                style.padding_left = Length::parse(tokens[3]);
            }
        } else if (p == "padding-top") style.padding_top = Length::parse(v);
        else if (p == "padding-right") style.padding_right = Length::parse(v);
        else if (p == "padding-bottom") style.padding_bottom = Length::parse(v);
        else if (p == "padding-left") style.padding_left = Length::parse(v);
        else if (p == "border" || p == "border-bottom" || p == "border-top") {
            // e.g. 1px solid #e1e4e8
            auto tokens = core::StringUtils::split_whitespace(v);
            if (!tokens.empty()) {
                float w = Length::parse(tokens[0]).resolve(0, style.font_size, 16.0f);
                style.border_top_width = style.border_right_width = style.border_bottom_width = style.border_left_width = w;
                if (tokens.size() > 1) {
                    if (tokens[1] == "solid") style.border_style = BorderStyle::SOLID;
                    else if (tokens[1] == "dashed") style.border_style = BorderStyle::DASHED;
                    else if (tokens[1] == "dotted") style.border_style = BorderStyle::DOTTED;
                }
                if (tokens.size() > 2) {
                    style.border_color = core::Color::parse(tokens[2]);
                }
            }
        } else if (p == "border-radius") {
            auto tokens = core::StringUtils::split_whitespace(v);
            if (!tokens.empty()) {
                float r = Length::parse(tokens[0]).resolve(0, style.font_size, 16.0f);
                style.border_radius = core::BorderRadius(r);
            }
        } else if (p == "box-shadow") {
            style.box_shadow = BoxShadow::parse(v);
        } else if (p == "flex-direction") {
            if (v == "column") style.flex_direction = FlexDirection::COLUMN;
            else if (v == "row-reverse") style.flex_direction = FlexDirection::ROW_REVERSE;
            else if (v == "column-reverse") style.flex_direction = FlexDirection::COLUMN_REVERSE;
            else style.flex_direction = FlexDirection::ROW;
        } else if (p == "justify-content") {
            if (v == "center") style.justify_content = JustifyContent::CENTER;
            else if (v == "flex-end") style.justify_content = JustifyContent::FLEX_END;
            else if (v == "space-between") style.justify_content = JustifyContent::SPACE_BETWEEN;
            else if (v == "space-around") style.justify_content = JustifyContent::SPACE_AROUND;
            else if (v == "space-evenly") style.justify_content = JustifyContent::SPACE_EVENLY;
            else style.justify_content = JustifyContent::FLEX_START;
        } else if (p == "align-items") {
            if (v == "center") style.align_items = AlignItems::CENTER;
            else if (v == "flex-start") style.align_items = AlignItems::FLEX_START;
            else if (v == "flex-end") style.align_items = AlignItems::FLEX_END;
            else if (v == "baseline") style.align_items = AlignItems::BASELINE;
            else style.align_items = AlignItems::STRETCH;
        } else if (p == "gap") {
            style.gap = Length::parse(v).resolve(0, style.font_size, 16.0f);
        } else if (p == "flex-grow") {
            try { style.flex_grow = std::stof(v); } catch (...) {}
        } else if (p == "flex-shrink") {
            try { style.flex_shrink = std::stof(v); } catch (...) {}
        } else if (p == "opacity") {
            try { style.opacity = std::stof(v); } catch (...) {}
        } else if (p == "z-index") {
            try { style.z_index = std::stoi(v); } catch (...) {}
        }
    }

public:
    CascadeEngine() {
        setup_user_agent_stylesheet();
    }

    void add_stylesheet(const StyleSheet& sheet) {
        stylesheets_.push_back(sheet);
    }

    void clear_stylesheets() {
        stylesheets_.clear();
    }

    ComputedStyle compute_style(const std::shared_ptr<html::Element>& elem, const ComputedStyle* parent_style = nullptr) {
        ComputedStyle style;

        // 1. Inherit properties from parent (CSS standard inheritance: color, font-family, font-size, line-height, text-align)
        if (parent_style) {
            style.color = parent_style->color;
            style.font_family = parent_style->font_family;
            style.font_size = parent_style->font_size;
            style.line_height = parent_style->line_height;
            style.text_align = parent_style->text_align;
        }

        // Struct to collect and sort matched declarations by specificity
        struct Candidate {
            Specificity specificity;
            std::string selector_str;
            std::string property;
            std::string value;
            bool important;
            size_t order;
        };

        std::vector<Candidate> candidates;
        size_t order = 0;

        // 2. Match User Agent Stylesheet
        for (const auto& rule : user_agent_sheet_.rules) {
            for (const auto& selector : rule.selectors) {
                if (matches_complex_selector(selector, elem)) {
                    Specificity spec = selector.calculate_specificity();
                    for (const auto& decl : rule.declarations) {
                        candidates.push_back({spec, "UA", decl.property, decl.value, decl.important, order++});
                    }
                }
            }
        }

        // 3. Match Author Stylesheets
        for (const auto& sheet : stylesheets_) {
            for (const auto& rule : sheet.rules) {
                for (const auto& selector : rule.selectors) {
                    if (matches_complex_selector(selector, elem)) {
                        Specificity spec = selector.calculate_specificity();
                        for (const auto& decl : rule.declarations) {
                            candidates.push_back({spec, "Author", decl.property, decl.value, decl.important, order++});
                        }
                    }
                }
            }
        }

        // 4. Inline Style Attribute (Specificity = (1, 0, 0, 0))
        if (elem->has_attribute("style")) {
            std::string inline_style_str = elem->get_attribute("style");
            auto inline_decls = CSSParser::parse_inline_style(inline_style_str);
            Specificity inline_spec(1, 0, 0, 0);
            for (const auto& decl : inline_decls) {
                candidates.push_back({inline_spec, "inline", decl.property, decl.value, decl.important, order++});
            }
        }

        // 5. Sort candidates by specificity and cascade order
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.important != b.important) {
                return !a.important && b.important;
            }
            if (a.specificity == b.specificity) {
                return a.order < b.order;
            }
            return a.specificity < b.specificity;
        });

        // 6. Apply winning declarations
        float parent_fs = parent_style ? parent_style->font_size : 16.0f;
        for (const auto& cand : candidates) {
            apply_declaration(style, cand.property, cand.value, parent_fs);
            style.matched_rules.push_back({cand.selector_str, cand.specificity, cand.property, cand.value, false});
        }

        return style;
    }
};

} // namespace nuby::css
