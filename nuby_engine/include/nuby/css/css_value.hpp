#pragma once

#include "../core/types.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <vector>
#include <cmath>

namespace nuby::css {

enum class Unit {
    PX,
    EM,
    REM,
    PERCENT,
    VH,
    VW,
    AUTO,
    NONE
};

struct Length {
    float value{0.0f};
    Unit unit{Unit::PX};

    Length() = default;
    Length(float val, Unit u) : value(val), unit(u) {}

    static Length px(float val) { return Length(val, Unit::PX); }
    static Length em(float val) { return Length(val, Unit::EM); }
    static Length rem(float val) { return Length(val, Unit::REM); }
    static Length percent(float val) { return Length(val, Unit::PERCENT); }
    static Length auto_val() { return Length(0.0f, Unit::AUTO); }
    static Length none_val() { return Length(0.0f, Unit::NONE); }

    bool is_auto() const { return unit == Unit::AUTO; }
    bool is_percent() const { return unit == Unit::PERCENT; }
    bool is_px() const { return unit == Unit::PX; }

    float resolve(float container_size, float font_size = 16.0f, float root_font_size = 16.0f) const {
        switch (unit) {
            case Unit::PX: return value;
            case Unit::EM: return value * font_size;
            case Unit::REM: return value * root_font_size;
            case Unit::PERCENT: return (value / 100.0f) * container_size;
            case Unit::VH: return (value / 100.0f) * 800.0f; // standard viewport height
            case Unit::VW: return (value / 100.0f) * 1200.0f; // standard viewport width
            case Unit::AUTO:
            case Unit::NONE:
            default: return 0.0f;
        }
    }

    static Length parse(const std::string& str);
};

enum class Display {
    BLOCK,
    INLINE,
    INLINE_BLOCK,
    FLEX,
    NONE
};

enum class Position {
    STATIC,
    RELATIVE,
    ABSOLUTE,
    FIXED
};

enum class FlexDirection {
    ROW,
    COLUMN,
    ROW_REVERSE,
    COLUMN_REVERSE
};

enum class JustifyContent {
    FLEX_START,
    FLEX_END,
    CENTER,
    SPACE_BETWEEN,
    SPACE_AROUND,
    SPACE_EVENLY
};

enum class AlignItems {
    FLEX_START,
    FLEX_END,
    CENTER,
    STRETCH,
    BASELINE
};

enum class TextAlign {
    LEFT,
    CENTER,
    RIGHT,
    JUSTIFY
};

enum class BorderStyle {
    NONE,
    SOLID,
    DASHED,
    DOTTED,
    DOUBLE
};

enum class BoxSizing {
    CONTENT_BOX,
    BORDER_BOX
};

struct BoxShadow {
    float offset_x{0.0f};
    float offset_y{0.0f};
    float blur_radius{0.0f};
    float spread_radius{0.0f};
    core::Color color{0, 0, 0, 80};
    bool inset{false};
    bool is_active{false};

    static BoxShadow parse(const std::string& str);
};

struct LinearGradient {
    float angle_deg{180.0f};
    std::vector<std::pair<float, core::Color>> stops; // (offset 0.0 to 1.0, color)
    bool is_active{false};

    static LinearGradient parse(const std::string& str);
};

} // namespace nuby::css
