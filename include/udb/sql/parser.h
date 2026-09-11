#pragma once

#include "udb/sql/ast.h"
#include "udb/sql/lexer.h"

namespace udb::sql {

// Parses exactly one statement, with an optional terminal semicolon.
// Syntax only: no catalog access, name resolution or schema validation.
class Parser {
public:
    static Statement Parse(std::string_view input);

private:
    explicit Parser(std::string_view input) : lexer_(input), current_(lexer_.Next()) {}
    Token Take(TokenType type, const char* expected);
    bool Match(TokenType type);
    CreateTableStatement CreateTable();
    CreateIndexStatement CreateIndex();
    DropTableStatement DropTable();
    InsertStatement Insert();
    SelectStatement Select();
    DeleteStatement Delete();
    UpdateStatement Update();
    Literal ParseLiteral();
    ExpressionPtr ParseExpression();
    ExpressionPtr ParseOr();
    ExpressionPtr ParseAnd();
    ExpressionPtr ParseNot();
    ExpressionPtr ParseComparison();
    ExpressionPtr ParsePrimary();

    Lexer lexer_;
    Token current_;
};

}  // namespace udb::sql
