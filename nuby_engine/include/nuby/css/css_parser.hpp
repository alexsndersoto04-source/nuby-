#pragma once

#include "css_rule.hpp"
#include "css_value.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <vector>
#include <sstream>

namespace nuby::css {

class CSSParser {
private:
    std::string css_;
    size_t cursor_{0};

    char peek() const {
        return cursor_ < css_.size() ? css_[cursor_] : '\0';
    }

    char next_char() {
        return cursor_ < css_.size() ? css_[cursor_++] : '\0';
    }

    void skip_whitespace_and_comments() {
        while (cursor_ < css_.size()) {
            if (std::isspace(static_cast<unsigned char>(css_[cursor_]))) {
                cursor_++;
            } else if (css_[cursor_] == '/' && cursor_ + 1 < css_.size() && css_[cursor_ + 1] == '*') {
                // Skip comment
                cursor_ += 2;
                while (cursor_ + 1 < css_.size() && !(css_[cursor_] == '*' && css_[cursor_ + 1] == '/')) {
                    cursor_++;
                }
                cursor_ = std::min(cursor_ + 2, css_.size());
            } else {
                break;
            }
        }
    }

    std::string consume_until(const std::string& chars) {
        std::string res;
        while (cursor_ < css_.size() && chars.find(css_[cursor_]) == std::string::npos) {
            res += next_char();
        }
        return res;
    }

public:
    explicit CSSParser(std::string css) : css_(std::move(css)) {}

    static std::vector<Declaration> parse_inline_style(const std::string& style_str) {
        CSSParser parser(style_str);
        return parser.parse_declarations_body();
    }

    std::vector<Declaration> parse_declarations_body() {
        std::vector<Declaration> declarations;
        while (cursor_ < css_.size()) {
            skip_whitespace_and_comments();
            if (cursor_ >= css_.size() || peek() == '}') break;

            std::string prop = core::StringUtils::trim(consume_until(":;}"));
            if (peek() == ':') {
                next_char(); // consume ':'
                skip_whitespace_and_comments();
                std::string val = core::StringUtils::trim(consume_until(";}"));
                
                bool important = false;
                if (core::StringUtils::ends_with(val, "!important")) {
                    important = true;
                    val = core::StringUtils::trim(val.substr(0, val.length() - 10));
                }

                if (!prop.empty()) {
                    declarations.push_back({core::StringUtils::to_lower(prop), val, important});
                }
            }
            if (peek() == ';') {
                next_char();
            }
        }
        return declarations;
    }

    // Construye UN SimpleSelector real a partir de un token de selector.
    // Cubre: *  tag  .clase  #id  :pseudo  [attr]  [attr="v"]  tag[attr]  tag[attr="v"]
    // (ANTES: los tokens con corchetes guardaban el texto CRUDO "[data-x]" y
    //  jamás casaban — la regla UA [data-nuby-ph] estaba muerta; bug latente.)
    static void flush_token(CompoundSelector& compound, std::string token) {
        auto bpos = token.find('[');
        if (bpos != std::string::npos && bpos != 0) {
            SimpleSelector t;
            t.type = SimpleSelectorType::TYPE;
            t.value = core::StringUtils::to_lower(token.substr(0, bpos));
            compound.simple_selectors.push_back(t);
            token = token.substr(bpos);
        }
        SimpleSelector simple;
        if (token[0] == '.')      { simple.type = SimpleSelectorType::CLASS;  simple.value = token.substr(1); }
        else if (token[0] == '#') { simple.type = SimpleSelectorType::ID;     simple.value = token.substr(1); }
        else if (token[0] == ':') { simple.type = SimpleSelectorType::PSEUDO_CLASS; simple.value = token.substr(1); }
        else if (token == "*")    { simple.type = SimpleSelectorType::UNIVERSAL; }
        else if (token[0] == '[') {
            std::string inner = token.substr(1);
            if (!inner.empty() && inner.back() == ']') inner.pop_back();
            auto eq = inner.find('=');
            if (eq != std::string::npos) {
                std::string name = core::StringUtils::trim(
                    core::StringUtils::to_lower(inner.substr(0, eq)));
                std::string val = core::StringUtils::trim(inner.substr(eq + 1));
                if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
                    val = val.substr(1, val.size() - 2);
                simple.type = SimpleSelectorType::ATTRIBUTE;
                simple.value = name + "=" + core::StringUtils::to_lower(val);
            } else {
                simple.type = SimpleSelectorType::ATTRIBUTE;
                simple.value = core::StringUtils::to_lower(inner);
            }
        } else {
            simple.type = SimpleSelectorType::TYPE;
            simple.value = core::StringUtils::to_lower(token);
        }
        compound.simple_selectors.push_back(simple);
    }

    ComplexSelector parse_complex_selector(const std::string& raw_sel) {
        ComplexSelector complex;
        std::string s = core::StringUtils::trim(raw_sel);
        if (s.empty()) return complex;

        // Split por combinadores '>', '+', '~', ' ' — pero NUNCA dentro de [...]
        CompoundSelector current_compound;
        std::string current_token;
        bool in_bracket = false;

        for (size_t i = 0; i < s.length(); ++i) {
            char c = s[i];
            if (c == '[') in_bracket = true;
            if (c == ']') in_bracket = false;
            if (!in_bracket && (c == ' ' || c == '>' || c == '+' || c == '~')) {
                if (!current_token.empty()) {
                    flush_token(current_compound, std::move(current_token));
                    current_token.clear();
                }

                if (c == '>') current_compound.combinator = Combinator::CHILD;
                else if (c == '+') current_compound.combinator = Combinator::ADJACENT_SIBLING;
                else if (c == '~') current_compound.combinator = Combinator::GENERAL_SIBLING;
                else if (c == ' ' && current_compound.combinator == Combinator::NONE) {
                    current_compound.combinator = Combinator::DESCENDANT;
                }

                if (!current_compound.simple_selectors.empty()) {
                    complex.compound_selectors.push_back(current_compound);
                    current_compound = CompoundSelector();
                }
            } else {
                current_token += c;
            }
        }

        if (!current_token.empty()) {
            flush_token(current_compound, std::move(current_token));
            complex.compound_selectors.push_back(current_compound);
        }

        return complex;
    }

    StyleSheet parse() {
        StyleSheet stylesheet;

        while (cursor_ < css_.size()) {
            skip_whitespace_and_comments();
            if (cursor_ >= css_.size()) break;

            // Handle @rules (e.g. @media, @keyframes) - skip for simplicity or parse
            if (peek() == '@') {
                consume_until("{");
                if (peek() == '{') next_char();
                int depth = 1;
                while (cursor_ < css_.size() && depth > 0) {
                    if (peek() == '{') depth++;
                    else if (peek() == '}') depth--;
                    next_char();
                }
                continue;
            }

            // Parse selectors
            std::string selectors_raw = core::StringUtils::trim(consume_until("{"));
            if (peek() != '{') break;
            next_char(); // consume '{'

            std::vector<std::string> raw_sel_list = core::StringUtils::split(selectors_raw, ',');
            std::vector<ComplexSelector> selectors;
            for (const auto& r : raw_sel_list) {
                std::string trimmed = core::StringUtils::trim(r);
                if (!trimmed.empty()) {
                    selectors.push_back(parse_complex_selector(trimmed));
                }
            }

            // Parse declarations
            std::vector<Declaration> declarations = parse_declarations_body();
            if (peek() == '}') next_char(); // consume '}'

            if (!selectors.empty() && !declarations.empty()) {
                stylesheet.rules.push_back({selectors, declarations});
            }
        }

        return stylesheet;
    }
};

} // namespace nuby::css
