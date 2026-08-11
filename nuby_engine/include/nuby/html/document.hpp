#pragma once

#include "element.hpp"
#include <string>
#include <memory>
#include <vector>

namespace nuby::html {

class Document : public Node {
private:
    std::string title_;
    std::string doctype_{"html"};
    std::shared_ptr<Element> root_element_; // <html>
    std::shared_ptr<Element> head_element_; // <head>
    std::shared_ptr<Element> body_element_; // <body>

public:
    Document() : Node(NodeType::DOCUMENT_NODE) {}

    const std::string& get_title() const { return title_; }
    void set_title(const std::string& title) { title_ = title; }

    const std::string& get_doctype() const { return doctype_; }
    void set_doctype(const std::string& doctype) { doctype_ = doctype; }

    std::shared_ptr<Element> get_root_element() const { return root_element_; }
    void set_root_element(std::shared_ptr<Element> root) {
        root_element_ = root;
        if (root) append_child(root);
    }

    std::shared_ptr<Element> get_head() const { return head_element_; }
    void set_head(std::shared_ptr<Element> head) { head_element_ = head; }

    std::shared_ptr<Element> get_body() const { return body_element_; }
    void set_body(std::shared_ptr<Element> body) { body_element_ = body; }

    std::shared_ptr<Element> create_element(const std::string& tag) {
        return std::make_shared<Element>(tag);
    }

    std::shared_ptr<TextNode> create_text_node(const std::string& text) {
        return std::make_shared<TextNode>(text);
    }

    std::shared_ptr<CommentNode> create_comment(const std::string& comment) {
        return std::make_shared<CommentNode>(comment);
    }

    std::shared_ptr<Element> get_element_by_id(const std::string& id) {
        if (!root_element_) return nullptr;
        return root_element_->get_element_by_id(id);
    }

    std::vector<std::shared_ptr<Element>> get_elements_by_tag_name(const std::string& tag) {
        if (!root_element_) return {};
        return root_element_->get_elements_by_tag_name(tag);
    }

    std::vector<std::shared_ptr<Element>> get_elements_by_class_name(const std::string& cls) {
        if (!root_element_) return {};
        return root_element_->get_elements_by_class_name(cls);
    }

    void set_text_content(const std::string&) override {}

    std::string to_string(int indent = 0) const override {
        std::string res = "<!DOCTYPE " + doctype_ + ">\n";
        if (root_element_) {
            res += root_element_->to_string(indent);
        }
        return res;
    }

    std::string to_json() const override {
        std::ostringstream ss;
        ss << "{\"doctype\":\"" << doctype_ << "\",\"title\":\"" << title_ << "\",\"root\":";
        if (root_element_) {
            ss << root_element_->to_json();
        } else {
            ss << "null";
        }
        ss << "}";
        return ss.str();
    }
};

} // namespace nuby::html
