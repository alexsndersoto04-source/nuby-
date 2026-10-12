#include "../../include/nuby/css/css_value.hpp"
#include "../../include/nuby/core/string_utils.hpp"
#include <sstream>
#include <cstdlib>

namespace nuby::css {

Length Length::parse(const std::string& str) {
    std::string s = core::StringUtils::trim(str);
    if (s.empty() || s == "auto") return Length::auto_val();
    if (s == "0") return Length::px(0.0f);
    if (s == "none") return Length::none_val();

    if (core::StringUtils::ends_with(s, "px")) {
        float val = std::stof(s.substr(0, s.length() - 2));
        return Length::px(val);
    } else if (core::StringUtils::ends_with(s, "%")) {
        float val = std::stof(s.substr(0, s.length() - 1));
        return Length::percent(val);
    } else if (core::StringUtils::ends_with(s, "rem")) {
        float val = std::stof(s.substr(0, s.length() - 3));
        return Length::rem(val);
    } else if (core::StringUtils::ends_with(s, "em")) {
        float val = std::stof(s.substr(0, s.length() - 2));
        return Length::em(val);
    } else if (core::StringUtils::ends_with(s, "vh")) {
        float val = std::stof(s.substr(0, s.length() - 2));
        return Length(val, Unit::VH);
    } else if (core::StringUtils::ends_with(s, "vw")) {
        float val = std::stof(s.substr(0, s.length() - 2));
        return Length(val, Unit::VW);
    }

    {
        // Sin excepciones: un valor no numérico no es una longitud válida
        char* end = nullptr;
        float val = std::strtof(s.c_str(), &end);
        if (end != s.c_str()) return Length::px(val);
        return Length::auto_val();
    }
}

BoxShadow BoxShadow::parse(const std::string& str) {
    BoxShadow shadow;
    std::string s = core::StringUtils::trim(str);
    if (s.empty() || s == "none") return shadow;

    auto tokens = core::StringUtils::split_whitespace(s);
    if (tokens.size() >= 2) {
        shadow.is_active = true;
        shadow.offset_x = Length::parse(tokens[0]).resolve(0);
        shadow.offset_y = Length::parse(tokens[1]).resolve(0);

        size_t color_idx = 2;
        if (tokens.size() >= 3 && !tokens[2].empty() && (std::isdigit(tokens[2][0]) || tokens[2][0] == '-')) {
            shadow.blur_radius = Length::parse(tokens[2]).resolve(0);
            color_idx = 3;
        }

        if (tokens.size() >= 4 && !tokens[3].empty() && (std::isdigit(tokens[3][0]) || tokens[3][0] == '-')) {
            shadow.spread_radius = Length::parse(tokens[3]).resolve(0);
            color_idx = 4;
        }

        if (tokens.size() > color_idx) {
            std::string color_part;
            for (size_t i = color_idx; i < tokens.size(); ++i) {
                if (i > color_idx) color_part += " ";
                color_part += tokens[i];
            }
            shadow.color = core::Color::parse(color_part);
        } else {
            shadow.color = core::Color(0, 0, 0, 80);
        }
    }
    return shadow;
}

LinearGradient LinearGradient::parse(const std::string& str) {
    LinearGradient grad;
    std::string s = core::StringUtils::trim(str);
    if (s.empty() || s.find("linear-gradient") == std::string::npos) return grad;

    size_t start = s.find('(');
    size_t end = s.rfind(')');
    if (start == std::string::npos || end == std::string::npos || end <= start) return grad;

    std::string inner = s.substr(start + 1, end - start - 1);
    auto parts = core::StringUtils::split(inner, ',');
    if (parts.size() >= 2) {
        grad.is_active = true;
        size_t stop_start = 0;
        std::string first = core::StringUtils::trim(parts[0]);

        if (core::StringUtils::ends_with(first, "deg")) {
            {
                std::string a = first.substr(0, first.length() - 3);
                char* end = nullptr;
                float v = std::strtof(a.c_str(), &end);
                if (end != a.c_str()) grad.angle_deg = v;
            }
            stop_start = 1;
        } else if (first == "to bottom") {
            grad.angle_deg = 180.0f;
            stop_start = 1;
        } else if (first == "to right") {
            grad.angle_deg = 90.0f;
            stop_start = 1;
        }

        size_t count = parts.size() - stop_start;
        for (size_t i = stop_start; i < parts.size(); ++i) {
            std::string stop_str = core::StringUtils::trim(parts[i]);
            auto stop_tokens = core::StringUtils::split_whitespace(stop_str);
            if (!stop_tokens.empty()) {
                core::Color c = core::Color::parse(stop_tokens[0]);
                float offset = (count > 1) ? static_cast<float>(i - stop_start) / (count - 1) : 0.0f;
                if (stop_tokens.size() > 1 && core::StringUtils::ends_with(stop_tokens[1], "%")) {
                    {
                        std::string pc = stop_tokens[1].substr(0, stop_tokens[1].length() - 1);
                        char* end = nullptr;
                        float v = std::strtof(pc.c_str(), &end);
                        if (end != pc.c_str()) offset = v / 100.0f;
                    }
                }
                grad.stops.push_back({offset, c});
            }
        }
    }
    return grad;
}

} // namespace nuby::css
