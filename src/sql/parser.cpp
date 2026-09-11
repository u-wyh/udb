#include "udb/sql/parser.h"

#include <charconv>
#include <limits>

namespace udb::sql {
namespace {

std::int64_t IntegerValue(const Token& token) {
    std::int64_t value = 0;
    const auto end = token.text.data() + token.text.size();
    const auto result = std::from_chars(token.text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw SqlError("Integer literal exceeds signed 64-bit range", token.position);
    }
    return value;
}

}  // namespace

Token Parser::Take(TokenType type, const char* expected) {
    if (current_.type != type) { throw SqlError(std::string("Expected ") + expected, current_.position); }
    auto token = current_;
    current_ = lexer_.Next();
    return token;
}

bool Parser::Match(TokenType type) {
    if (current_.type != type) { return false; }
    current_ = lexer_.Next();
    return true;
}

Statement Parser::Parse(std::string_view input) {
    Parser parser(input);
    Statement statement;
    switch (parser.current_.type) {
        case TokenType::Create:
            parser.Take(TokenType::Create, "CREATE");
            if (parser.current_.type == TokenType::Table) { statement = parser.CreateTable(); }
            else if (parser.current_.type == TokenType::Index) { statement = parser.CreateIndex(); }
            else { throw SqlError("Expected TABLE or INDEX", parser.current_.position); }
            break;
        case TokenType::Drop:
            parser.Take(TokenType::Drop, "DROP");
            if (parser.current_.type == TokenType::Table) { statement = parser.DropTable(); }
            else if (parser.current_.type == TokenType::Index) { statement = parser.DropIndex(); }
            else { throw SqlError("Expected TABLE or INDEX", parser.current_.position); }
            break;
        case TokenType::Insert: statement = parser.Insert(); break;
        case TokenType::Select: statement = parser.Select(); break;
        case TokenType::Delete: statement = parser.Delete(); break;
        case TokenType::Update: statement = parser.Update(); break;
        default: throw SqlError("Expected CREATE, DROP, INSERT, SELECT, DELETE or UPDATE", parser.current_.position);
    }
    parser.Match(TokenType::Semicolon);
    parser.Take(TokenType::End, "end of input");
    return statement;
}

CreateTableStatement Parser::CreateTable() {
    Take(TokenType::Table, "TABLE");
    CreateTableStatement statement;
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    Take(TokenType::LeftParen, "(");
    do {
        ColumnDefinition column;
        column.name = Take(TokenType::Identifier, "column name").text;
        if (Match(TokenType::Integer)) { column.type = TypeId::INTEGER; }
        else if (Match(TokenType::BigInt)) { column.type = TypeId::BIGINT; }
        else if (Match(TokenType::Boolean)) { column.type = TypeId::BOOLEAN; }
        else if (Match(TokenType::Varchar)) {
            column.type = TypeId::VARCHAR;
            Take(TokenType::LeftParen, "(");
            const auto token = Take(TokenType::IntegerLiteral, "VARCHAR length");
            const auto length = IntegerValue(token);
            if (length <= 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max()) {
                throw SqlError("VARCHAR length must be in 1..4294967295", token.position);
            }
            column.max_length = static_cast<std::uint32_t>(length);
            Take(TokenType::RightParen, ")");
        } else {
            throw SqlError("Expected column type", current_.position);
        }
        statement.columns.push_back(std::move(column));
    } while (Match(TokenType::Comma));
    Take(TokenType::RightParen, ")");
    return statement;
}

CreateIndexStatement Parser::CreateIndex() {
    Take(TokenType::Index, "INDEX");
    CreateIndexStatement statement;
    statement.index_name = Take(TokenType::Identifier, "index name").text;
    Take(TokenType::On, "ON");
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    Take(TokenType::LeftParen, "(");
    statement.column_name = Take(TokenType::Identifier, "column name").text;
    Take(TokenType::RightParen, ")");
    return statement;
}

DropTableStatement Parser::DropTable() {
    Take(TokenType::Table, "TABLE");
    return {Take(TokenType::Identifier, "table name").text};
}

DropIndexStatement Parser::DropIndex() {
    Take(TokenType::Index, "INDEX");
    return {Take(TokenType::Identifier, "index name").text};
}

Literal Parser::ParseLiteral() {
    switch (current_.type) {
        case TokenType::IntegerLiteral: return IntegerValue(Take(TokenType::IntegerLiteral, "integer"));
        case TokenType::StringLiteral: return Take(TokenType::StringLiteral, "string").text;
        case TokenType::True: Take(TokenType::True, "TRUE"); return true;
        case TokenType::False: Take(TokenType::False, "FALSE"); return false;
        case TokenType::Null: Take(TokenType::Null, "NULL"); return std::monostate{};
        default: throw SqlError("Expected literal", current_.position);
    }
}

InsertStatement Parser::Insert() {
    Take(TokenType::Insert, "INSERT");
    Take(TokenType::Into, "INTO");
    InsertStatement statement;
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    Take(TokenType::Values, "VALUES");
    Take(TokenType::LeftParen, "(");
    do { statement.values.push_back(ParseLiteral()); } while (Match(TokenType::Comma));
    Take(TokenType::RightParen, ")");
    return statement;
}

SelectStatement Parser::Select() {
    Take(TokenType::Select, "SELECT");
    SelectStatement statement;
    if (Match(TokenType::Star)) {
        statement.select_all = true;
    } else {
        do { statement.column_names.push_back(Take(TokenType::Identifier, "column name").text); }
        while (Match(TokenType::Comma));
    }
    Take(TokenType::From, "FROM");
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    if (Match(TokenType::Where)) { statement.predicate = ParseExpression(); }
    if (Match(TokenType::Order)) {
        Take(TokenType::By, "BY");
        do {
            OrderBy order{Take(TokenType::Identifier, "column name").text, true};
            if (Match(TokenType::Desc)) { order.ascending = false; }
            else { Match(TokenType::Asc); }
            statement.order_by.push_back(std::move(order));
        } while (Match(TokenType::Comma));
    }
    if (Match(TokenType::Limit)) {
        const auto token = Take(TokenType::IntegerLiteral, "nonnegative LIMIT");
        const auto limit = IntegerValue(token);
        if (limit < 0 || static_cast<std::uint64_t>(limit) >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw SqlError("LIMIT is outside the supported range", token.position);
        }
        statement.limit = static_cast<std::size_t>(limit);
        if (Match(TokenType::Offset)) {
            const auto offset_token = Take(TokenType::IntegerLiteral, "nonnegative OFFSET");
            const auto offset = IntegerValue(offset_token);
            if (offset < 0 || static_cast<std::uint64_t>(offset) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                throw SqlError("OFFSET is outside the supported range", offset_token.position);
            }
            statement.offset = static_cast<std::size_t>(offset);
        }
    }
    return statement;
}

DeleteStatement Parser::Delete() {
    Take(TokenType::Delete, "DELETE");
    Take(TokenType::From, "FROM");
    DeleteStatement statement;
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    if (Match(TokenType::Where)) { statement.predicate = ParseExpression(); }
    return statement;
}

UpdateStatement Parser::Update() {
    Take(TokenType::Update, "UPDATE");
    UpdateStatement statement;
    statement.table_name = Take(TokenType::Identifier, "table name").text;
    Take(TokenType::Set, "SET");
    do {
        UpdateAssignment assignment;
        assignment.column_name = Take(TokenType::Identifier, "column name").text;
        Take(TokenType::Equal, "=");
        assignment.value = ParseLiteral();
        statement.assignments.push_back(std::move(assignment));
    } while (Match(TokenType::Comma));
    if (Match(TokenType::Where)) { statement.predicate = ParseExpression(); }
    return statement;
}

ExpressionPtr Parser::ParseExpression() { return ParseOr(); }

ExpressionPtr Parser::ParseOr() {
    auto left = ParseAnd();
    while (Match(TokenType::Or)) {
        left = std::make_shared<Expression>(LogicalExpression{LogicalOperator::Or, left, ParseAnd()});
    }
    return left;
}

ExpressionPtr Parser::ParseAnd() {
    auto left = ParseNot();
    while (Match(TokenType::And)) {
        left = std::make_shared<Expression>(LogicalExpression{LogicalOperator::And, left, ParseNot()});
    }
    return left;
}

ExpressionPtr Parser::ParseNot() {
    if (Match(TokenType::Not)) {
        return std::make_shared<Expression>(LogicalExpression{LogicalOperator::Not, ParseNot(), nullptr});
    }
    return ParseComparison();
}

ExpressionPtr Parser::ParseComparison() {
    auto left = ParsePrimary();
    ComparisonOperator op;
    if (Match(TokenType::Equal)) { op = ComparisonOperator::Equal; }
    else if (Match(TokenType::NotEqual)) { op = ComparisonOperator::NotEqual; }
    else if (Match(TokenType::Less)) { op = ComparisonOperator::Less; }
    else if (Match(TokenType::LessEqual)) { op = ComparisonOperator::LessEqual; }
    else if (Match(TokenType::Greater)) { op = ComparisonOperator::Greater; }
    else if (Match(TokenType::GreaterEqual)) { op = ComparisonOperator::GreaterEqual; }
    else { return left; }
    return std::make_shared<Expression>(ComparisonExpression{op, left, ParsePrimary()});
}

ExpressionPtr Parser::ParsePrimary() {
    if (Match(TokenType::LeftParen)) {
        auto expression = ParseExpression();
        Take(TokenType::RightParen, ")");
        return expression;
    }
    if (current_.type == TokenType::Identifier) {
        return std::make_shared<Expression>(ColumnExpression{Take(TokenType::Identifier, "column").text});
    }
    if (current_.type == TokenType::IntegerLiteral || current_.type == TokenType::StringLiteral ||
        current_.type == TokenType::True || current_.type == TokenType::False || current_.type == TokenType::Null) {
        return std::make_shared<Expression>(LiteralExpression{ParseLiteral()});
    }
    throw SqlError("Expected expression", current_.position);
}

}  // namespace udb::sql
