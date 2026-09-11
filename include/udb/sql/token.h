#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace udb::sql {

struct SourcePosition {
    std::size_t offset = 0;  // Byte offset, zero-based.
    std::size_t line = 1;
    std::size_t column = 1;  // Byte column, one-based.
};

class SqlError : public std::runtime_error {
public:
    SqlError(const std::string& message, SourcePosition position)
        : std::runtime_error(message + " at line " + std::to_string(position.line) +
              ", column " + std::to_string(position.column) + " (byte " + std::to_string(position.offset) + ")"),
          position_(position) {}
    SourcePosition Position() const { return position_; }
private:
    SourcePosition position_;
};

enum class TokenType {
    End, Identifier, IntegerLiteral, StringLiteral,
    LeftParen, RightParen, Comma, Semicolon, Star,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    Create, Table, Insert, Into, Values, Select, Delete, Update, Set, From, Where,
    And, Or, Not,
    Integer, BigInt, Boolean, Varchar, True, False, Null
};

struct Token {
    TokenType type;
    std::string text;  // Original spelling, except strings have SQL quotes decoded.
    SourcePosition position;
};

}  // namespace udb::sql
