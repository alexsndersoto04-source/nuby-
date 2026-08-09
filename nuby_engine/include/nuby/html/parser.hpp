#pragma once

#include "tokenizer.hpp"
#include "document.hpp"
#include "element.hpp"
#include "../core/string_utils.hpp"
#include <vector>
#include <memory>
#include <unordered_set>

namespace nuby::html {

class HTMLParser {
private:
    std::string html_;
    std::shared_ptr<Document> document_;
    std::vector<std::shared_ptr<Element>> open_elements_;

    static const std::unordered_set<std::string>& void_elements() {
        static const std::unordered_set<std::string> voids = {
            "area", "base", "br", "col", "embed", "hr", "img",
            "input", "link", "meta", "param", "source", "track", "wbr"
        };
        return voids;
    }

    bool is_void_element(const std::string& tag) const {
        return void_elements().find(core::StringUtils::to_lower(tag)) != void_elements().end();
    }

    std::shared_ptr<Element> current_node() const {
        return open_elements_.empty() ? nullptr : open_elements_.back();
    }

    void pop_until(const std::string& tag_name) {
        std::string lower_tag = core::StringUtils::to_lower(tag_name);
        while (!open_elements_.empty()) {
            auto elem = open_elements_.back();
            open_elements_.pop_back();
            if (elem->get_tag_name() == lower_tag) {
                break;
            }
        }
    }

public:
    explicit HTMLParser(std::string html) : html_(std::move(html)) {
        document_ = std::make_shared<Document>();
    }

    std::shared_ptr<Document> parse() {
        HTMLTokenizer tokenizer(html_);
        auto tokens = tokenizer.tokenize();

        std::shared_ptr<Element> html_root = nullptr;
        std::shared_ptr<Element> head_elem = nullptr;
        std::shared_ptr<Element> body_elem = nullptr;

        for (const auto& tok : tokens) {
            switch (tok.type) {
                case HTMLTokenType::DOCTYPE: {
                    document_->set_doctype(tok.data);
                    break;
                }

                case HTMLTokenType::COMMENT: {
                    auto comment = document_->create_comment(tok.data);
                    if (current_node()) {
                        current_node()->append_child(comment);
                    } else {
                        document_->append_child(comment);
                    }
                    break;
                }

                case HTMLTokenType::START_TAG: {
                    auto elem = document_->create_element(tok.tag_name);
                    for (const auto& [attr_name, attr_val] : tok.attributes) {
                        elem->set_attribute(attr_name, attr_val);
                    }

                    if (tok.tag_name == "html" && !html_root) {
                        html_root = elem;
                        document_->set_root_element(elem);
                    } else if (tok.tag_name == "head" && !head_elem) {
                        head_elem = elem;
                        document_->set_head(elem);
                    } else if (tok.tag_name == "body" && !body_elem) {
                        body_elem = elem;
                        document_->set_body(elem);
                    }

                    if (current_node()) {
                        current_node()->append_child(elem);
                    } else {
                        if (!html_root && tok.tag_name != "html") {
                            html_root = document_->create_element("html");
                            document_->set_root_element(html_root);
                            open_elements_.push_back(html_root);

                            body_elem = document_->create_element("body");
                            document_->set_body(body_elem);
                            html_root->append_child(body_elem);
                            open_elements_.push_back(body_elem);

                            body_elem->append_child(elem);
                        } else if (html_root && tok.tag_name != "html") {
                            html_root->append_child(elem);
                        }
                    }

                    // Void elements don't go to open_elements stack
                    if (!is_void_element(tok.tag_name) && !tok.self_closing) {
                        open_elements_.push_back(elem);
                    }
                    break;
                }

                case HTMLTokenType::END_TAG: {
                    if (is_void_element(tok.tag_name)) {
                        break;
                    }
                    pop_until(tok.tag_name);
                    break;
                }

                case HTMLTokenType::CHARACTER: {
                    if (tok.data.empty()) break;
                    
                    // If no root element yet, create one
                    if (open_elements_.empty()) {
                        if (!html_root) {
                            html_root = document_->create_element("html");
                            document_->set_root_element(html_root);
                            open_elements_.push_back(html_root);

                            body_elem = document_->create_element("body");
                            document_->set_body(body_elem);
                            html_root->append_child(body_elem);
                            open_elements_.push_back(body_elem);
                        }
                    }

                    auto text_node = document_->create_text_node(tok.data);
                    if (current_node()) {
                        // Extract <title> content if inside head
                        if (current_node()->get_tag_name() == "title") {
                            document_->set_title(tok.data);
                        }
                        current_node()->append_child(text_node);
                    }
                    break;
                }

                case HTMLTokenType::END_OF_FILE:
                    break;
            }
        }

        // Guarantee basic HTML tree structure if missing
        if (!document_->get_root_element()) {
            auto html = document_->create_element("html");
            auto body = document_->create_element("body");
            html->append_child(body);
            document_->set_root_element(html);
            document_->set_body(body);
        }

        return document_;
    }
};

} // namespace nuby::html
