#pragma once

#include "css_value.hpp"
#include "../core/types.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <vector>
#include <tuple>
#include <memory>

namespace nuby::css {

struct Specificity {
    int inline_style{0};
    int ids{0};
    int classes_and_pseudos{0};
    int elements{0};

    Specificity() = default;
    Specificity(int a, int b, int c, int d)
        : inline_style(a), ids(b), classes_and_pseudos(c), elements(d) {}

    bool operator<(const Specificity& other) const {
        return std::tie(inline_style, ids, classes_and_pseudos, elements) <
               std::tie(other.inline_style, other.ids, other.classes_and_pseudos, other.elements);
    }

    bool operator>(const Specificity& other) const {
        return other < *this;
    }

    bool operator==(const Specificity& other) const {
        return inline_style == other.inline_style &&
               ids == other.ids &&
               classes_and_pseudos == other.classes_and_pseudos &&
               elements == other.elements;
    }

    std::string to_string() const {
        return "(" + std::to_string(inline_style) + ", " +
               std::to_string(ids) + ", " +
               std::to_string(classes_and_pseudos) + ", " +
               std::to_string(elements) + ")";
    }
};

enum class SimpleSelectorType {
    UNIVERSAL,
    TYPE,           // div, p, span
    CLASS,          // .container
    ID,             // #main
    ATTRIBUTE,      // [target="_blank"]
    PSEUDO_CLASS    // :hover, :first-child
};

struct SimpleSelector {
    SimpleSelectorType type{SimpleSelectorType::UNIVERSAL};
    std::string value;
    std::string attr_value;

    Specificity calculate_specificity() const {
        switch (type) {
            case SimpleSelectorType::ID:
                return Specificity(0, 1, 0, 0);
            case SimpleSelectorType::CLASS:
            case SimpleSelectorType::ATTRIBUTE:
            case SimpleSelectorType::PSEUDO_CLASS:
                return Specificity(0, 0, 1, 0);
            case SimpleSelectorType::TYPE:
                return Specificity(0, 0, 0, 1);
            case SimpleSelectorType::UNIVERSAL:
            default:
                return Specificity(0, 0, 0, 0);
        }
    }
};

enum class Combinator {
    NONE,
    DESCENDANT,     // ' '
    CHILD,          // '>'
    ADJACENT_SIBLING, // '+'
    GENERAL_SIBLING   // '~'
};

struct CompoundSelector {
    std::vector<SimpleSelector> simple_selectors;
    Combinator combinator{Combinator::NONE};

    Specificity calculate_specificity() const {
        Specificity spec;
        for (const auto& s : simple_selectors) {
            auto sub = s.calculate_specificity();
            spec.ids += sub.ids;
            spec.classes_and_pseudos += sub.classes_and_pseudos;
            spec.elements += sub.elements;
        }
        return spec;
    }
};

struct ComplexSelector {
    std::vector<CompoundSelector> compound_selectors;

    Specificity calculate_specificity() const {
        Specificity total;
        for (const auto& compound : compound_selectors) {
            auto s = compound.calculate_specificity();
            total.ids += s.ids;
            total.classes_and_pseudos += s.classes_and_pseudos;
            total.elements += s.elements;
        }
        return total;
    }

    std::string to_string() const;
};

struct Declaration {
    std::string property;
    std::string value;
    bool important{false};
};

struct Rule {
    std::vector<ComplexSelector> selectors;
    std::vector<Declaration> declarations;
};

struct StyleSheet {
    std::vector<Rule> rules;
};

} // namespace nuby::css
