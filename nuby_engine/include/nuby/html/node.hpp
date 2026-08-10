#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <iostream>
#include <functional>

namespace nuby::html {

enum class NodeType {
    ELEMENT_NODE = 1,
    TEXT_NODE = 3,
    COMMENT_NODE = 8,
    DOCUMENT_NODE = 9,
    DOCUMENT_TYPE_NODE = 10
};

class Element;
class TextNode;

class Node : public std::enable_shared_from_this<Node> {
protected:
    NodeType type_;
    std::weak_ptr<Node> parent_;
    std::vector<std::shared_ptr<Node>> children_;

public:
    explicit Node(NodeType type) : type_(type) {}
    virtual ~Node() = default;

    NodeType get_node_type() const { return type_; }
    bool is_element() const { return type_ == NodeType::ELEMENT_NODE; }
    bool is_text() const { return type_ == NodeType::TEXT_NODE; }
    bool is_comment() const { return type_ == NodeType::COMMENT_NODE; }
    bool is_document() const { return type_ == NodeType::DOCUMENT_NODE; }

    std::shared_ptr<Node> get_parent() const { return parent_.lock(); }
    void set_parent(std::shared_ptr<Node> p) { parent_ = p; }

    const std::vector<std::shared_ptr<Node>>& get_children() const { return children_; }
    
    void append_child(std::shared_ptr<Node> child) {
        if (!child) return;
        child->set_parent(shared_from_this());
        children_.push_back(child);
    }

    void remove_child(std::shared_ptr<Node> child) {
        auto it = std::find(children_.begin(), children_.end(), child);
        if (it != children_.end()) {
            (*it)->set_parent(nullptr);
            children_.erase(it);
        }
    }

    void clear_children() {
        for (auto& child : children_) {
            if (child) child->set_parent(nullptr);
        }
        children_.clear();
    }

    std::shared_ptr<Node> first_child() const {
        return children_.empty() ? nullptr : children_.front();
    }

    std::shared_ptr<Node> last_child() const {
        return children_.empty() ? nullptr : children_.back();
    }

    std::shared_ptr<Node> next_sibling() const {
        auto p = get_parent();
        if (!p) return nullptr;
        const auto& sibs = p->get_children();
        for (size_t i = 0; i < sibs.size(); ++i) {
            if (sibs[i].get() == this && i + 1 < sibs.size()) {
                return sibs[i + 1];
            }
        }
        return nullptr;
    }

    std::shared_ptr<Node> previous_sibling() const {
        auto p = get_parent();
        if (!p) return nullptr;
        const auto& sibs = p->get_children();
        for (size_t i = 0; i < sibs.size(); ++i) {
            if (sibs[i].get() == this && i > 0) {
                return sibs[i - 1];
            }
        }
        return nullptr;
    }

    virtual std::string get_text_content() const {
        std::string text;
        for (const auto& child : children_) {
            text += child->get_text_content();
        }
        return text;
    }

    virtual void set_text_content(const std::string& text) = 0;

    virtual std::string to_string(int indent = 0) const = 0;
    virtual std::string to_json() const = 0;

    void traverse(const std::function<void(std::shared_ptr<Node>)>& callback) {
        callback(shared_from_this());
        for (const auto& child : children_) {
            child->traverse(callback);
        }
    }
};

class TextNode : public Node {
private:
    std::string text_;

public:
    explicit TextNode(std::string text)
        : Node(NodeType::TEXT_NODE), text_(std::move(text)) {}

    const std::string& get_text() const { return text_; }
    void set_text(const std::string& text) { text_ = text; }

    std::string get_text_content() const override { return text_; }
    void set_text_content(const std::string& text) override { text_ = text; }

    std::string to_string(int indent = 0) const override {
        std::string ind(indent * 2, ' ');
        return ind + "#text: \"" + text_ + "\"\n";
    }

    std::string to_json() const override {
        std::string escaped;
        for (char c : text_) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else escaped += c;
        }
        return "{\"type\":\"text\",\"text\":\"" + escaped + "\"}";
    }
};

class CommentNode : public Node {
private:
    std::string comment_;

public:
    explicit CommentNode(std::string comment)
        : Node(NodeType::COMMENT_NODE), comment_(std::move(comment)) {}

    const std::string& get_comment() const { return comment_; }
    std::string get_text_content() const override { return ""; }
    void set_text_content(const std::string&) override {}

    std::string to_string(int indent = 0) const override {
        std::string ind(indent * 2, ' ');
        return ind + "<!-- " + comment_ + " -->\n";
    }

    std::string to_json() const override {
        return "{\"type\":\"comment\",\"comment\":\"" + comment_ + "\"}";
    }
};

} // namespace nuby::html
