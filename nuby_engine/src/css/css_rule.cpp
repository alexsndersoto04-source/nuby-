#include "../../include/nuby/css/css_rule.hpp"

namespace nuby::css {

std::string ComplexSelector::to_string() const {
    std::string res;
    for (size_t i = 0; i < compound_selectors.size(); ++i) {
        const auto& comp = compound_selectors[i];
        for (const auto& s : comp.simple_selectors) {
            switch (s.type) {
                case SimpleSelectorType::TYPE: res += s.value; break;
                case SimpleSelectorType::CLASS: res += "." + s.value; break;
                case SimpleSelectorType::ID: res += "#" + s.value; break;
                case SimpleSelectorType::PSEUDO_CLASS: res += ":" + s.value; break;
                case SimpleSelectorType::ATTRIBUTE: res += s.value; break;
                case SimpleSelectorType::UNIVERSAL: res += "*"; break;
            }
        }
        if (i + 1 < compound_selectors.size()) {
            switch (comp.combinator) {
                case Combinator::CHILD: res += " > "; break;
                case Combinator::ADJACENT_SIBLING: res += " + "; break;
                case Combinator::GENERAL_SIBLING: res += " ~ "; break;
                case Combinator::DESCENDANT:
                default: res += " "; break;
            }
        }
    }
    return res;
}

} // namespace nuby::css
