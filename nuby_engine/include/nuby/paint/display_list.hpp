#pragma once

#include "../core/types.hpp"
#include "../layout/text_shaper.hpp"
#include <vector>
#include <string>
#include <memory>
#include <sstream>

namespace nuby::paint {

enum class CommandType {
    FILL_RECT,
    FILL_ROUNDED_RECT,
    DRAW_BORDER,
    DRAW_BOX_SHADOW,
    DRAW_LINEAR_GRADIENT,
    DRAW_TEXT,
    PUSH_CLIP,
    POP_CLIP
};

struct DrawCommand {
    CommandType type{CommandType::FILL_RECT};
    core::RectF rect;
    core::Color color;
    core::BorderRadius radius{0.0f};

    // Border specific
    core::Edges border_widths;
    core::Color border_color;

    // Shadow specific
    float blur_radius{0.0f};
    float spread_radius{0.0f};
    core::PointF shadow_offset;

    // Gradient specific
    float gradient_angle{180.0f};
    std::vector<std::pair<float, core::Color>> gradient_stops;

    // Text specific
    std::string text;
    float font_size{16.0f};
    int font_weight{400};

    int z_index{0};

    std::string to_string() const {
        std::ostringstream ss;
        switch (type) {
            case CommandType::FILL_RECT:
                ss << "FillRect(" << rect.x << ", " << rect.y << ", " << rect.width << "x" << rect.height << ", " << color.to_hex_string() << ")";
                break;
            case CommandType::FILL_ROUNDED_RECT:
                ss << "FillRoundedRect(" << rect.x << ", " << rect.y << ", " << rect.width << "x" << rect.height << ", r=" << radius.top_left << ")";
                break;
            case CommandType::DRAW_BORDER:
                ss << "DrawBorder(" << rect.x << ", " << rect.y << ", " << rect.width << "x" << rect.height << ")";
                break;
            case CommandType::DRAW_BOX_SHADOW:
                ss << "DrawBoxShadow(" << rect.x << ", " << rect.y << ", blur=" << blur_radius << ")";
                break;
            case CommandType::DRAW_LINEAR_GRADIENT:
                ss << "DrawLinearGradient(" << rect.x << ", " << rect.y << ", angle=" << gradient_angle << ")";
                break;
            case CommandType::DRAW_TEXT:
                ss << "DrawText(\"" << text << "\", " << rect.x << ", " << rect.y << ", " << color.to_hex_string() << ")";
                break;
            case CommandType::PUSH_CLIP:
                ss << "PushClip(" << rect.x << ", " << rect.y << ", " << rect.width << "x" << rect.height << ")";
                break;
            case CommandType::POP_CLIP:
                ss << "PopClip()";
                break;
        }
        return ss.str();
    }
};

class DisplayList {
private:
    std::vector<DrawCommand> commands_;

public:
    void clear() { commands_.clear(); }

    void add_command(const DrawCommand& cmd) {
        commands_.push_back(cmd);
    }

    const std::vector<DrawCommand>& get_commands() const { return commands_; }
    size_t size() const { return commands_.size(); }

    std::string to_json() const {
        std::ostringstream ss;
        ss << "[\n";
        for (size_t i = 0; i < commands_.size(); ++i) {
            const auto& cmd = commands_[i];
            ss << "  {\"type\":\"" << static_cast<int>(cmd.type) << "\", "
               << "\"x\":" << cmd.rect.x << ", \"y\":" << cmd.rect.y << ", "
               << "\"w\":" << cmd.rect.width << ", \"h\":" << cmd.rect.height << ", "
               << "\"color\":\"" << cmd.color.to_hex_string() << "\", "
               << "\"text\":\"" << cmd.text << "\", "
               << "\"desc\":\"" << cmd.to_string() << "\"}"
               << (i + 1 < commands_.size() ? ",\n" : "\n");
        }
        ss << "]";
        return ss.str();
    }
};

} // namespace nuby::paint
