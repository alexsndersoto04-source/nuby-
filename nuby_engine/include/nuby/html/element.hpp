#pragma once

#include "node.hpp"
#include "../core/string_utils.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <sstream>

namespace nuby::html {

class Element : public Node {
private:
    std::string tag_name_;
    std::unordered_map<std::string, std::string> attributes_;
    std::vector<std::string> class_list_;

    void update_class_list() {
        class_list_.clear();
        auto it = attributes_.find("class");
        if (it != attributes_.end()) {
            class_list_ = core::StringUtils::split_whitespace(it->second);
        }
    }

public:
    explicit Element(std::string tag_name)
        : Node(NodeType::ELEMENT_NODE), tag_name_(core::StringUtils::to_lower(tag_name)) {}

    const std::string& get_tag_name() const { return tag_name_; }

    const std::unordered_map<std::string, std::string>& get_attributes() const {
        return attributes_;
    }

    bool has_attribute(const std::string& name) const {
        return attributes_.find(core::StringUtils::to_lower(name)) != attributes_.end();
    }

    std::string get_attribute(const std::string& name) const {
        auto it = attributes_.find(core::StringUtils::to_lower(name));
        return it != attributes_.end() ? it->second : "";
    }

    void set_attribute(const std::string& name, const std::string& value) {
        std::string lower_name = core::StringUtils::to_lower(name);
        attributes_[lower_name] = value;
        if (lower_name == "class") {
            update_class_list();
        }
    }

    void remove_attribute(const std::string& name) {
        std::string lower_name = core::StringUtils::to_lower(name);
        attributes_.erase(lower_name);
        if (lower_name == "class") {
            class_list_.clear();
        }
    }

    std::string get_id() const {
        return get_attribute("id");
    }

    const std::vector<std::string>& get_class_list() const {
        return class_list_;
    }

    bool has_class(const std::string& cls) const {
        for (const auto& c : class_list_) {
            if (c == cls) return true;
        }
        return false;
    }

    void add_class(const std::string& cls) {
        if (!has_class(cls)) {
            class_list_.push_back(cls);
            std::string combined;
            for (size_t i = 0; i < class_list_.size(); ++i) {
                if (i > 0) combined += " ";
                combined += class_list_[i];
            }
            attributes_["class"] = combined;
        }
    }

    void remove_class(const std::string& cls) {
        auto it = std::find(class_list_.begin(), class_list_.end(), cls);
        if (it != class_list_.end()) {
            class_list_.erase(it);
            std::string combined;
            for (size_t i = 0; i < class_list_.size(); ++i) {
                if (i > 0) combined += " ";
                combined += class_list_[i];
            }
            attributes_["class"] = combined;
        }
    }

    void set_text_content(const std::string& text) override {
        children_.clear();
        if (!text.empty()) {
            append_child(std::make_shared<TextNode>(text));
        }
    }

    // DOM Traversal Helpers
    std::shared_ptr<Element> get_element_by_id(const std::string& id) {
        if (get_id() == id) return std::static_pointer_cast<Element>(shared_from_this());
        for (const auto& child : children_) {
            if (child->is_element()) {
                auto elem = std::static_pointer_cast<Element>(child);
                auto res = elem->get_element_by_id(id);
                if (res) return res;
            }
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<Element>> get_elements_by_tag_name(const std::string& tag) {
        std::vector<std::shared_ptr<Element>> result;
        std::string lower_tag = core::StringUtils::to_lower(tag);
        if (tag_name_ == lower_tag || lower_tag == "*") {
            result.push_back(std::static_pointer_cast<Element>(shared_from_this()));
        }
        for (const auto& child : children_) {
            if (child->is_element()) {
                auto elem = std::static_pointer_cast<Element>(child);
                auto sub = elem->get_elements_by_tag_name(tag);
                result.insert(result.end(), sub.begin(), sub.end());
            }
        }
        return result;
    }

    std::vector<std::shared_ptr<Element>> get_elements_by_class_name(const std::string& cls) {
        std::vector<std::shared_ptr<Element>> result;
        if (has_class(cls)) {
            result.push_back(std::static_pointer_cast<Element>(shared_from_this()));
        }
        for (const auto& child : children_) {
            if (child->is_element()) {
                auto elem = std::static_pointer_cast<Element>(child);
                auto sub = elem->get_elements_by_class_name(cls);
                result.insert(result.end(), sub.begin(), sub.end());
            }
        }
        return result;
    }

    std::string to_string(int indent = 0) const override {
        std::string ind(indent * 2, ' ');
        std::string res = ind + "<" + tag_name_;
        for (const auto& [k, v] : attributes_) {
            res += " " + k + "=\"" + v + "\"";
        }
        res += ">\n";
        for (const auto& child : children_) {
            res += child->to_string(indent + 1);
        }
        res += ind + "</" + tag_name_ + ">\n";
        return res;
    }

    std::string to_json() const override {
        std::ostringstream ss;
        ss << "{\"type\":\"element\",\"tag\":\"" << tag_name_ << "\",";
        ss << "\"attributes\":{";
        size_t count = 0;
        for (const auto& [k, v] : attributes_) {
            if (count++ > 0) ss << ",";
            ss << "\"" << k << "\":\"";
            for (char c : v) {
                if (c == '"') ss << "\\\"";
                else if (c == '\\') ss << "\\\\";
                else ss << c;
            }
            ss << "\"";
        }
        ss << "},\"children\":[";
        for (size_t i = 0; i < children_.size(); ++i) {
            if (i > 0) ss << ",";
            ss << children_[i]->to_json();
        }
        ss << "]}";
        return ss.str();
    }
};

} // namespace nuby::html
