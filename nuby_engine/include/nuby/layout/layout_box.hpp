#pragma once

#include "box_model.hpp"
#include "text_shaper.hpp"
#include "../css/cascade.hpp"
#include "../html/node.hpp"
#include <memory>
#include <vector>
#include <string>
#include <sstream>

namespace nuby::layout {

enum class BoxType {
    BLOCK_BOX,
    INLINE_BOX,
    INLINE_BLOCK_BOX,
    FLEX_CONTAINER_BOX,
    FLEX_ITEM_BOX,
    ANONYMOUS_BLOCK_BOX,
    TEXT_BOX
};

class LayoutBox : public std::enable_shared_from_this<LayoutBox> {
public:
    BoxType box_type{BoxType::BLOCK_BOX};
    BoxDimensions dimensions;
    css::ComputedStyle style;
    std::shared_ptr<html::Node> node;
    std::weak_ptr<LayoutBox> parent;
    std::vector<std::shared_ptr<LayoutBox>> children;
    std::vector<TextRun> text_runs;

    explicit LayoutBox(BoxType type) : box_type(type) {}

    void append_child(std::shared_ptr<LayoutBox> child) {
        if (!child) return;
        child->parent = shared_from_this();
        children.push_back(child);
    }

    std::shared_ptr<LayoutBox> get_parent() const {
        return parent.lock();
    }

    bool is_block_level() const {
        return box_type == BoxType::BLOCK_BOX ||
               box_type == BoxType::FLEX_CONTAINER_BOX ||
               box_type == BoxType::ANONYMOUS_BLOCK_BOX;
    }

    bool is_inline_level() const {
        return box_type == BoxType::INLINE_BOX ||
               box_type == BoxType::INLINE_BLOCK_BOX ||
               box_type == BoxType::TEXT_BOX;
    }

    bool is_flex_container() const {
        return box_type == BoxType::FLEX_CONTAINER_BOX;
    }

    std::string to_string(int indent = 0) const {
        std::string ind(indent * 2, ' ');
        std::ostringstream ss;
        ss << ind << "[Box: ";
        switch (box_type) {
            case BoxType::BLOCK_BOX: ss << "BLOCK"; break;
            case BoxType::INLINE_BOX: ss << "INLINE"; break;
            case BoxType::INLINE_BLOCK_BOX: ss << "INLINE_BLOCK"; break;
            case BoxType::FLEX_CONTAINER_BOX: ss << "FLEX"; break;
            case BoxType::FLEX_ITEM_BOX: ss << "FLEX_ITEM"; break;
            case BoxType::ANONYMOUS_BLOCK_BOX: ss << "ANONYMOUS"; break;
            case BoxType::TEXT_BOX: ss << "TEXT"; break;
        }
        ss << "] (" << dimensions.content.x << ", " << dimensions.content.y
           << ", " << dimensions.content.width << "x" << dimensions.content.height << ")\n";
        for (const auto& child : children) {
            ss << child->to_string(indent + 1);
        }
        return ss.str();
    }

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"type\": \"";
        switch (box_type) {
            case BoxType::BLOCK_BOX: ss << "BLOCK"; break;
            case BoxType::INLINE_BOX: ss << "INLINE"; break;
            case BoxType::INLINE_BLOCK_BOX: ss << "INLINE_BLOCK"; break;
            case BoxType::FLEX_CONTAINER_BOX: ss << "FLEX"; break;
            case BoxType::FLEX_ITEM_BOX: ss << "FLEX_ITEM"; break;
            case BoxType::ANONYMOUS_BLOCK_BOX: ss << "ANONYMOUS"; break;
            case BoxType::TEXT_BOX: ss << "TEXT"; break;
        }
        ss << "\",\n";
        ss << "  \"x\": " << dimensions.content.x << ",\n";
        ss << "  \"y\": " << dimensions.content.y << ",\n";
        ss << "  \"width\": " << dimensions.content.width << ",\n";
        ss << "  \"height\": " << dimensions.content.height << ",\n";
        ss << "  \"margin\": {\"top\":" << dimensions.margin.top << ",\"right\":" << dimensions.margin.right
           << ",\"bottom\":" << dimensions.margin.bottom << ",\"left\":" << dimensions.margin.left << "},\n";
        ss << "  \"padding\": {\"top\":" << dimensions.padding.top << ",\"right\":" << dimensions.padding.right
           << ",\"bottom\":" << dimensions.padding.bottom << ",\"left\":" << dimensions.padding.left << "},\n";
        ss << "  \"border\": {\"top\":" << dimensions.border.top << ",\"right\":" << dimensions.border.right
           << ",\"bottom\":" << dimensions.border.bottom << ",\"left\":" << dimensions.border.left << "},\n";
        ss << "  \"children\": [\n";
        for (size_t i = 0; i < children.size(); ++i) {
            ss << children[i]->to_json() << (i + 1 < children.size() ? ",\n" : "\n");
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }
};

} // namespace nuby::layout
