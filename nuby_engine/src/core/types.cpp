#include "../../include/nuby/core/types.hpp"
#include "../../include/nuby/core/string_utils.hpp"
#include <unordered_map>
#include <sstream>

namespace nuby::core {

Color Color::parse(const std::string& str) {
    std::string s = StringUtils::to_lower(StringUtils::trim(str));
    if (s.empty()) return Color::black();

    // Hex colors #fff or #ffffff or #ffffffff
    if (s[0] == '#') {
        return Color::from_hex(s);
    }

    // rgb(r, g, b) or rgba(r, g, b, a)
    if (StringUtils::starts_with(s, "rgb(") || StringUtils::starts_with(s, "rgba(")) {
        size_t start = s.find('(');
        size_t end = s.find(')');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            std::string content = s.substr(start + 1, end - start - 1);
            auto parts = StringUtils::split(content, ',');
            if (parts.size() >= 3) {
                try {
                    uint8_t r = static_cast<uint8_t>(std::clamp(std::stoi(StringUtils::trim(parts[0])), 0, 255));
                    uint8_t g = static_cast<uint8_t>(std::clamp(std::stoi(StringUtils::trim(parts[1])), 0, 255));
                    uint8_t b = static_cast<uint8_t>(std::clamp(std::stoi(StringUtils::trim(parts[2])), 0, 255));
                    uint8_t a = 255;
                    if (parts.size() >= 4) {
                        float af = std::stof(StringUtils::trim(parts[3]));
                        a = static_cast<uint8_t>(std::clamp(af * 255.0f, 0.0f, 255.0f));
                    }
                    return Color(r, g, b, a);
                } catch (...) {}
            }
        }
    }

    // Standard Named CSS Colors
    static const std::unordered_map<std::string, Color> named_colors = {
        {"transparent", Color(0, 0, 0, 0)},
        {"white", Color(255, 255, 255, 255)},
        {"black", Color(0, 0, 0, 255)},
        {"red", Color(255, 0, 0, 255)},
        {"green", Color(0, 128, 0, 255)},
        {"blue", Color(0, 0, 255, 255)},
        {"yellow", Color(255, 255, 0, 255)},
        {"purple", Color(128, 0, 128, 255)},
        {"orange", Color(255, 165, 0, 255)},
        {"gray", Color(128, 128, 128, 255)},
        {"grey", Color(128, 128, 128, 255)},
        {"darkgray", Color(169, 169, 169, 255)},
        {"lightgray", Color(211, 211, 211, 255)},
        {"cyan", Color(0, 255, 255, 255)},
        {"magenta", Color(255, 0, 255, 255)},
        {"navy", Color(0, 0, 128, 255)},
        {"teal", Color(0, 128, 128, 255)},
        {"coral", Color(255, 127, 80, 255)},
        {"crimson", Color(220, 20, 60, 255)},
        {"gold", Color(255, 215, 0, 255)},
        {"indigo", Color(75, 0, 130, 255)},
        {"violet", Color(238, 130, 238, 255)},
        {"khaki", Color(240, 230, 140, 255)},
        {"salmon", Color(250, 128, 114, 255)},
        {"plum", Color(221, 160, 221, 255)},
        {"skyblue", Color(135, 206, 235, 255)},
        {"royalblue", Color(65, 105, 225, 255)},
        {"dodgerblue", Color(30, 144, 255, 255)},
        {"deepskyblue", Color(0, 191, 255, 255)},
        {"slateblue", Color(106, 90, 205, 255)},
        {"darkblue", Color(0, 0, 139, 255)},
        {"midnightblue", Color(25, 25, 112, 255)},
        {"limegreen", Color(50, 205, 50, 255)},
        {"forestgreen", Color(34, 139, 34, 255)},
        {"seagreen", Color(46, 139, 87, 255)},
        {"darkgreen", Color(0, 100, 0, 255)},
        {"olive", Color(128, 128, 0, 255)},
        {"maroon", Color(128, 0, 0, 255)},
        {"brown", Color(165, 42, 42, 255)},
        {"pink", Color(255, 192, 203, 255)},
        {"hotpink", Color(255, 105, 180, 255)},
        {"deeppink", Color(255, 20, 147, 255)}
    };

    auto it = named_colors.find(s);
    if (it != named_colors.end()) {
        return it->second;
    }

    return Color::black();
}

} // namespace nuby::core
