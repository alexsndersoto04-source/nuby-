#pragma once

#include "../core/types.hpp"
#include <algorithm>

namespace nuby::layout {

struct BoxDimensions {
    core::RectF content;
    core::Edges padding;
    core::Edges border;
    core::Edges margin;

    // Helper rectangles
    core::RectF padding_box() const {
        return core::RectF(
            content.x - padding.left,
            content.y - padding.top,
            content.width + padding.horizontal(),
            content.height + padding.vertical()
        );
    }

    core::RectF border_box() const {
        auto pad = padding_box();
        return core::RectF(
            pad.x - border.left,
            pad.y - border.top,
            pad.width + border.horizontal(),
            pad.height + border.vertical()
        );
    }

    core::RectF margin_box() const {
        auto b = border_box();
        return core::RectF(
            b.x - margin.left,
            b.y - margin.top,
            b.width + margin.horizontal(),
            b.height + margin.vertical()
        );
    }
};

} // namespace nuby::layout
