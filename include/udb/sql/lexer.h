#pragma once

#include "udb/sql/token.h"

#include <string_view>

namespace udb::sql {

// ASCII identifiers: [A-Za-z_][A-Za-z0-9_]*. Strings preserve arbitrary bytes.
// Integer tokens may have an immediately preceding minus; comments are unsupported.
class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}
    Token Next();

private:
    char Advance();
    std::string input_;
    SourcePosition position_;
};

}  // namespace udb::sql
