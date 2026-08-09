#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include "../core/string_utils.hpp"

namespace nuby::html {

enum class HTMLTokenType {
    DOCTYPE,
    START_TAG,
    END_TAG,
    COMMENT,
    CHARACTER,
    END_OF_FILE
};

struct HTMLToken {
    HTMLTokenType type{HTMLTokenType::CHARACTER};
    std::string tag_name;
    std::unordered_map<std::string, std::string> attributes;
    std::string current_attr_name;
    std::string current_attr_val;
    std::string data;
    bool self_closing{false};

    void clear() {
        type = HTMLTokenType::CHARACTER;
        tag_name.clear();
        attributes.clear();
        current_attr_name.clear();
        current_attr_val.clear();
        data.clear();
        self_closing = false;
    }

    void commit_attribute() {
        if (!current_attr_name.empty()) {
            attributes[current_attr_name] = current_attr_val;
            current_attr_name.clear();
            current_attr_val.clear();
        }
    }
};

enum class TokenizerState {
    DATA,
    CHARACTER_REFERENCE,
    TAG_OPEN,
    END_TAG_OPEN,
    TAG_NAME,
    BEFORE_ATTRIBUTE_NAME,
    ATTRIBUTE_NAME,
    AFTER_ATTRIBUTE_NAME,
    BEFORE_ATTRIBUTE_VALUE,
    ATTRIBUTE_VALUE_DOUBLE_QUOTED,
    ATTRIBUTE_VALUE_SINGLE_QUOTED,
    ATTRIBUTE_VALUE_UNQUOTED,
    AFTER_ATTRIBUTE_VALUE_QUOTED,
    SELF_CLOSING_START_TAG,
    BOGUS_COMMENT,
    MARKUP_DECLARATION_OPEN,
    COMMENT_START,
    COMMENT_START_DASH,
    COMMENT,
    COMMENT_END_DASH,
    COMMENT_END,
    DOCTYPE_STATE,
    RAWTEXT
};

class HTMLTokenizer {
private:
    std::string input_;
    size_t cursor_{0};
    TokenizerState state_{TokenizerState::DATA};
    HTMLToken current_token_;
    std::vector<HTMLToken> tokens_;
    std::string rawtext_tag_;

    char peek() const {
        return cursor_ < input_.size() ? input_[cursor_] : '\0';
    }

    char next_char() {
        return cursor_ < input_.size() ? input_[cursor_++] : '\0';
    }

    void reconsume() {
        if (cursor_ > 0) cursor_--;
    }

    void emit_token(const HTMLToken& token) {
        tokens_.push_back(token);
    }

    void emit_character(char c) {
        if (!tokens_.empty() && tokens_.back().type == HTMLTokenType::CHARACTER) {
            tokens_.back().data += c;
        } else {
            HTMLToken tok;
            tok.type = HTMLTokenType::CHARACTER;
            tok.data = std::string(1, c);
            tokens_.push_back(tok);
        }
    }

    void decode_named_entity(std::string& text) {
        // Common entities
        static const std::unordered_map<std::string, std::string> entities = {
            {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
            {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "},
            {"&copy;", "©"}, {"&reg;", "®"}, {"&trade;", "™"},
            {"&mdash;", "—"}, {"&ndash;", "–"}, {"&bull;", "•"},
            {"&hellip;", "…"}, {"&euro;", "€"}, {"&pound;", "£"},
            {"&yen;", "¥"}
        };
        for (const auto& [entity, replacement] : entities) {
            size_t pos = 0;
            while ((pos = text.find(entity, pos)) != std::string::npos) {
                text.replace(pos, entity.length(), replacement);
                pos += replacement.length();
            }
        }
    }

public:
    explicit HTMLTokenizer(std::string input) : input_(std::move(input)) {}

    std::vector<HTMLToken> tokenize() {
        tokens_.clear();
        cursor_ = 0;
        state_ = TokenizerState::DATA;

        while (cursor_ <= input_.size()) {
            char c = next_char();

            switch (state_) {
                case TokenizerState::DATA: {
                    if (c == '&') {
                        // Lookahead for entity
                        std::string entity = "&";
                        while (cursor_ < input_.size() && input_[cursor_] != ';' &&
                               input_[cursor_] != ' ' && input_[cursor_] != '<' &&
                               entity.length() < 10) {
                            entity += next_char();
                        }
                        if (cursor_ < input_.size() && input_[cursor_] == ';') {
                            entity += next_char();
                        }
                        decode_named_entity(entity);
                        for (char ec : entity) emit_character(ec);
                    } else if (c == '<') {
                        state_ = TokenizerState::TAG_OPEN;
                    } else if (c == '\0') {
                        HTMLToken eof;
                        eof.type = HTMLTokenType::END_OF_FILE;
                        emit_token(eof);
                        return tokens_;
                    } else {
                        emit_character(c);
                    }
                    break;
                }

                case TokenizerState::TAG_OPEN: {
                    if (c == '!') {
                        state_ = TokenizerState::MARKUP_DECLARATION_OPEN;
                    } else if (c == '/') {
                        state_ = TokenizerState::END_TAG_OPEN;
                    } else if (std::isalpha(static_cast<unsigned char>(c))) {
                        current_token_.clear();
                        current_token_.type = HTMLTokenType::START_TAG;
                        current_token_.tag_name += std::tolower(static_cast<unsigned char>(c));
                        state_ = TokenizerState::TAG_NAME;
                    } else if (c == '?') {
                        state_ = TokenizerState::BOGUS_COMMENT;
                    } else {
                        emit_character('<');
                        reconsume();
                        state_ = TokenizerState::DATA;
                    }
                    break;
                }

                case TokenizerState::END_TAG_OPEN: {
                    if (std::isalpha(static_cast<unsigned char>(c))) {
                        current_token_.clear();
                        current_token_.type = HTMLTokenType::END_TAG;
                        current_token_.tag_name += std::tolower(static_cast<unsigned char>(c));
                        state_ = TokenizerState::TAG_NAME;
                    } else if (c == '>') {
                        state_ = TokenizerState::DATA;
                    } else {
                        state_ = TokenizerState::BOGUS_COMMENT;
                    }
                    break;
                }

                case TokenizerState::TAG_NAME: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_NAME;
                    } else if (c == '/') {
                        state_ = TokenizerState::SELF_CLOSING_START_TAG;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        
                        // Handle rawtext elements like <style> and <script>
                        if (current_token_.type == HTMLTokenType::START_TAG &&
                            (current_token_.tag_name == "style" || current_token_.tag_name == "script")) {
                            rawtext_tag_ = current_token_.tag_name;
                            state_ = TokenizerState::RAWTEXT;
                        } else {
                            state_ = TokenizerState::DATA;
                        }
                    } else if (c == '\0') {
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.tag_name += std::tolower(static_cast<unsigned char>(c));
                    }
                    break;
                }

                case TokenizerState::RAWTEXT: {
                    // Collect raw text until </style> or </script>
                    std::string end_tag = "</" + rawtext_tag_ + ">";
                    std::string raw_content;
                    reconsume(); // start from current position

                    size_t found = input_.find(end_tag, cursor_);
                    if (found == std::string::npos) {
                        // Case-insensitive search
                        std::string upper_end = "</" + rawtext_tag_;
                        found = input_.find(upper_end, cursor_);
                    }

                    if (found != std::string::npos) {
                        raw_content = input_.substr(cursor_, found - cursor_);
                        cursor_ = found + end_tag.length();

                        if (!raw_content.empty()) {
                            HTMLToken char_tok;
                            char_tok.type = HTMLTokenType::CHARACTER;
                            char_tok.data = raw_content;
                            emit_token(char_tok);
                        }

                        HTMLToken end_tok;
                        end_tok.type = HTMLTokenType::END_TAG;
                        end_tok.tag_name = rawtext_tag_;
                        emit_token(end_tok);

                        state_ = TokenizerState::DATA;
                    } else {
                        // Till end of file
                        raw_content = input_.substr(cursor_);
                        cursor_ = input_.size() + 1;
                        if (!raw_content.empty()) {
                            HTMLToken char_tok;
                            char_tok.type = HTMLTokenType::CHARACTER;
                            char_tok.data = raw_content;
                            emit_token(char_tok);
                        }
                        state_ = TokenizerState::DATA;
                    }
                    break;
                }

                case TokenizerState::BEFORE_ATTRIBUTE_NAME: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        // Ignore whitespace
                    } else if (c == '/') {
                        state_ = TokenizerState::SELF_CLOSING_START_TAG;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else if (c == '\0') {
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.commit_attribute();
                        current_token_.current_attr_name = std::string(1, std::tolower(static_cast<unsigned char>(c)));
                        state_ = TokenizerState::ATTRIBUTE_NAME;
                    }
                    break;
                }

                case TokenizerState::ATTRIBUTE_NAME: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        state_ = TokenizerState::AFTER_ATTRIBUTE_NAME;
                    } else if (c == '=') {
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_VALUE;
                    } else if (c == '/') {
                        current_token_.commit_attribute();
                        state_ = TokenizerState::SELF_CLOSING_START_TAG;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        current_token_.current_attr_name += std::tolower(static_cast<unsigned char>(c));
                    }
                    break;
                }

                case TokenizerState::AFTER_ATTRIBUTE_NAME: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        // ignore
                    } else if (c == '=') {
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_VALUE;
                    } else if (c == '/') {
                        current_token_.commit_attribute();
                        state_ = TokenizerState::SELF_CLOSING_START_TAG;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        current_token_.commit_attribute();
                        current_token_.current_attr_name = std::string(1, std::tolower(static_cast<unsigned char>(c)));
                        state_ = TokenizerState::ATTRIBUTE_NAME;
                    }
                    break;
                }

                case TokenizerState::BEFORE_ATTRIBUTE_VALUE: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        // ignore
                    } else if (c == '"') {
                        state_ = TokenizerState::ATTRIBUTE_VALUE_DOUBLE_QUOTED;
                    } else if (c == '\'') {
                        state_ = TokenizerState::ATTRIBUTE_VALUE_SINGLE_QUOTED;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        current_token_.current_attr_val += c;
                        state_ = TokenizerState::ATTRIBUTE_VALUE_UNQUOTED;
                    }
                    break;
                }

                case TokenizerState::ATTRIBUTE_VALUE_DOUBLE_QUOTED: {
                    if (c == '"') {
                        state_ = TokenizerState::AFTER_ATTRIBUTE_VALUE_QUOTED;
                    } else if (c == '\0') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.current_attr_val += c;
                    }
                    break;
                }

                case TokenizerState::ATTRIBUTE_VALUE_SINGLE_QUOTED: {
                    if (c == '\'') {
                        state_ = TokenizerState::AFTER_ATTRIBUTE_VALUE_QUOTED;
                    } else if (c == '\0') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.current_attr_val += c;
                    }
                    break;
                }

                case TokenizerState::ATTRIBUTE_VALUE_UNQUOTED: {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        current_token_.commit_attribute();
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_NAME;
                    } else if (c == '>') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else if (c == '\0') {
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.current_attr_val += c;
                    }
                    break;
                }

                case TokenizerState::AFTER_ATTRIBUTE_VALUE_QUOTED: {
                    current_token_.commit_attribute();
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_NAME;
                    } else if (c == '/') {
                        state_ = TokenizerState::SELF_CLOSING_START_TAG;
                    } else if (c == '>') {
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        reconsume();
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_NAME;
                    }
                    break;
                }

                case TokenizerState::SELF_CLOSING_START_TAG: {
                    if (c == '>') {
                        current_token_.self_closing = true;
                        current_token_.commit_attribute();
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        reconsume();
                        state_ = TokenizerState::BEFORE_ATTRIBUTE_NAME;
                    }
                    break;
                }

                case TokenizerState::MARKUP_DECLARATION_OPEN: {
                    if (c == '-' && peek() == '-') {
                        next_char(); // consume second '-'
                        current_token_.clear();
                        current_token_.type = HTMLTokenType::COMMENT;
                        state_ = TokenizerState::COMMENT;
                    } else if (core::StringUtils::to_lower(std::string(1, c)) == "d") {
                        // DOCTYPE lookahead
                        std::string doc = "d";
                        while (cursor_ < input_.size() && input_[cursor_] != ' ' && input_[cursor_] != '>') {
                            doc += next_char();
                        }
                        current_token_.clear();
                        current_token_.type = HTMLTokenType::DOCTYPE;
                        state_ = TokenizerState::DOCTYPE_STATE;
                    } else {
                        state_ = TokenizerState::BOGUS_COMMENT;
                    }
                    break;
                }

                case TokenizerState::COMMENT: {
                    if (c == '-') {
                        state_ = TokenizerState::COMMENT_END_DASH;
                    } else if (c == '\0') {
                        emit_token(current_token_);
                        return tokens_;
                    } else {
                        current_token_.data += c;
                    }
                    break;
                }

                case TokenizerState::COMMENT_END_DASH: {
                    if (c == '-') {
                        state_ = TokenizerState::COMMENT_END;
                    } else {
                        current_token_.data += '-';
                        current_token_.data += c;
                        state_ = TokenizerState::COMMENT;
                    }
                    break;
                }

                case TokenizerState::COMMENT_END: {
                    if (c == '>') {
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else if (c == '-') {
                        current_token_.data += '-';
                    } else {
                        current_token_.data += "--";
                        current_token_.data += c;
                        state_ = TokenizerState::COMMENT;
                    }
                    break;
                }

                case TokenizerState::DOCTYPE_STATE: {
                    if (c == '>') {
                        emit_token(current_token_);
                        state_ = TokenizerState::DATA;
                    } else {
                        current_token_.data += c;
                    }
                    break;
                }

                case TokenizerState::BOGUS_COMMENT: {
                    if (c == '>' || c == '\0') {
                        state_ = TokenizerState::DATA;
                    }
                    break;
                }

                default:
                    state_ = TokenizerState::DATA;
                    break;
            }
        }

        return tokens_;
    }
};

} // namespace nuby::html
