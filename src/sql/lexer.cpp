#include "udb/sql/lexer.h"

#include <unordered_map>

namespace udb::sql {
namespace {

bool Letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

const std::unordered_map<std::string, TokenType> kKeywords = {
    {"CREATE", TokenType::Create}, {"DROP", TokenType::Drop}, {"TABLE", TokenType::Table},
    {"INDEX", TokenType::Index}, {"ON", TokenType::On},
    {"INSERT", TokenType::Insert}, {"INTO", TokenType::Into}, {"VALUES", TokenType::Values},
    {"SELECT", TokenType::Select}, {"DELETE", TokenType::Delete},
    {"UPDATE", TokenType::Update}, {"SET", TokenType::Set},
    {"FROM", TokenType::From}, {"WHERE", TokenType::Where}, {"LIMIT", TokenType::Limit},
    {"AND", TokenType::And}, {"OR", TokenType::Or}, {"NOT", TokenType::Not},
    {"INTEGER", TokenType::Integer}, {"BIGINT", TokenType::BigInt},
    {"BOOLEAN", TokenType::Boolean}, {"VARCHAR", TokenType::Varchar},
    {"TRUE", TokenType::True}, {"FALSE", TokenType::False}, {"NULL", TokenType::Null}
};

}  // namespace

char Lexer::Advance() {
    const auto c = input_[position_.offset++];
    if (c == '\n' || c == '\r') {
        // Treat CRLF as one line break; lone CR and lone LF also start a line.
        if (c != '\n' || position_.offset < 2 || input_[position_.offset - 2] != '\r') {
            ++position_.line;
        }
        position_.column = 1;
    } else {
        ++position_.column;
    }
    return c;
}

Token Lexer::Next() {
    while (position_.offset < input_.size() && Space(input_[position_.offset])) { Advance(); }
    const auto start = position_;
    if (start.offset == input_.size()) { return {TokenType::End, "", start}; }
    const auto c = Advance();
    if (Letter(c)) {
        while (position_.offset < input_.size() && (Letter(input_[position_.offset]) || Digit(input_[position_.offset]))) {
            Advance();
        }
        const auto text = input_.substr(start.offset, position_.offset - start.offset);
        auto uppercase = text;
        for (auto& letter : uppercase) {
            if (letter >= 'a' && letter <= 'z') { letter = static_cast<char>(letter - 'a' + 'A'); }
        }
        const auto keyword = kKeywords.find(uppercase);
        return {keyword == kKeywords.end() ? TokenType::Identifier : keyword->second, text, start};
    }
    if (Digit(c) || (c == '-' && position_.offset < input_.size() && Digit(input_[position_.offset]))) {
        while (position_.offset < input_.size() && Digit(input_[position_.offset])) { Advance(); }
        return {TokenType::IntegerLiteral, input_.substr(start.offset, position_.offset - start.offset), start};
    }
    if (c == '\'') {
        std::string text;
        while (position_.offset < input_.size()) {
            const auto next = Advance();
            if (next == '\'') {
                if (position_.offset < input_.size() && input_[position_.offset] == '\'') {
                    Advance();
                    text.push_back('\'');
                } else {
                    return {TokenType::StringLiteral, std::move(text), start};
                }
            } else {
                text.push_back(next);
            }
        }
        throw SqlError("Unterminated string literal", start);
    }
    switch (c) {
        case '(': return {TokenType::LeftParen, "(", start};
        case ')': return {TokenType::RightParen, ")", start};
        case ',': return {TokenType::Comma, ",", start};
        case ';': return {TokenType::Semicolon, ";", start};
        case '*': return {TokenType::Star, "*", start};
        case '=': return {TokenType::Equal, "=", start};
        case '!':
            if (position_.offset < input_.size() && input_[position_.offset] == '=') {
                Advance();
                return {TokenType::NotEqual, "!=", start};
            }
            throw SqlError("Expected = after !", start);
        case '<':
            if (position_.offset < input_.size() && input_[position_.offset] == '=') {
                Advance();
                return {TokenType::LessEqual, "<=", start};
            }
            return {TokenType::Less, "<", start};
        case '>':
            if (position_.offset < input_.size() && input_[position_.offset] == '=') {
                Advance();
                return {TokenType::GreaterEqual, ">=", start};
            }
            return {TokenType::Greater, ">", start};
        default: throw SqlError("Illegal character", start);
    }
}

}  // namespace udb::sql
