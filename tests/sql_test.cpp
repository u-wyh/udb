#include "udb/sql/parser.h"

#include <iostream>
#include <limits>
#include <random>

namespace {

using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const SqlError& error) {
        Check(std::string(error.what()).find("line ") != std::string::npos &&
              std::string(error.what()).find("column ") != std::string::npos, "Error lacks location");
        return;
    }
    throw std::runtime_error("Expected SQL error");
}

void TestLexer() {
    Lexer keywords("CREATE table InSeRt into VALUES select FROM integer BIGINT Boolean varchar TRUE false NuLl");
    for (const auto type : {TokenType::Create, TokenType::Table, TokenType::Insert, TokenType::Into,
         TokenType::Values, TokenType::Select, TokenType::From, TokenType::Integer, TokenType::BigInt,
         TokenType::Boolean, TokenType::Varchar, TokenType::True, TokenType::False, TokenType::Null}) {
        Check(keywords.Next().type == type, "Keyword token mismatch");
    }
    Check(keywords.Next().type == TokenType::End && keywords.Next().type == TokenType::End, "EOF unstable");
    Lexer tokens(" \t\r\nMy_Id123\f\v(42,-7);* 'It''s OK' '' 'a\\b'");
    const auto id = tokens.Next();
    Check(id.type == TokenType::Identifier && id.text == "My_Id123", "Identifier spelling lost");
    Check(id.position.offset == 4 && id.position.line == 2 && id.position.column == 1, "CRLF position incorrect");
    Check(tokens.Next().type == TokenType::LeftParen, "Missing left parenthesis");
    Check(tokens.Next().text == "42", "Integer token text wrong");
    Check(tokens.Next().type == TokenType::Comma, "Missing comma");
    const auto negative = tokens.Next();
    Check(negative.type == TokenType::IntegerLiteral && negative.text == "-7", "Signed integer token wrong");
    Check(tokens.Next().type == TokenType::RightParen && tokens.Next().type == TokenType::Semicolon &&
          tokens.Next().type == TokenType::Star, "Punctuation mismatch");
    Check(tokens.Next().text == "It's OK" && tokens.Next().text.empty() && tokens.Next().text == "a\\b", "String decoding failed");
    Lexer unicode(u8"'你好'");
    Check(unicode.Next().text == u8"你好", "String bytes changed");
    Lexer binary(std::string("'a\0b'", 5));
    Check(binary.Next().text == std::string("a\0b", 3), "Embedded NUL string bytes lost");
    Lexer quote("''''");
    Check(quote.Next().text == "'", "Escaped quote failed");
    Lexer reserved_prefix("SELECTED _from from2");
    for (int i = 0; i < 3; ++i) { Check(reserved_prefix.Next().type == TokenType::Identifier, "Keyword prefix misclassified"); }
    for (const auto input : {"@", "#", "\"name\"", "-", "'unterminated", "'''"}) {
        Reject([&] { Lexer lexer(input); lexer.Next(); });
    }
    Reject([] { Lexer lexer(std::string(1, '\0')); lexer.Next(); });
    try {
        Lexer lexer("\n  'unfinished");
        lexer.Next();
        throw std::runtime_error("Expected unterminated string error");
    } catch (const SqlError& error) {
        Check(error.Position().line == 2 && error.Position().column == 3 && error.Position().offset == 3,
              "String error location incorrect");
    }
}

void TestCreate() {
    const auto single = std::get<CreateTableStatement>(Parser::Parse("CREATE TABLE T (id INTEGER)"));
    Check(single.table_name == "T" && single.columns.size() == 1 && single.columns[0].name == "id" &&
          single.columns[0].type == udb::TypeId::INTEGER && single.columns[0].max_length == 0, "Single column AST wrong");
    const auto multi = std::get<CreateTableStatement>(Parser::Parse(
        "cReAtE TABLE Users (id INTEGER, score BIGINT, Name VARCHAR(100), active BOOLEAN);"));
    Check(multi.table_name == "Users" && multi.columns.size() == 4 && multi.columns[1].type == udb::TypeId::BIGINT &&
          multi.columns[2].name == "Name" && multi.columns[2].type == udb::TypeId::VARCHAR &&
          multi.columns[2].max_length == 100 && multi.columns[3].type == udb::TypeId::BOOLEAN, "Multi column AST wrong");
    Check(std::get<CreateTableStatement>(Parser::Parse("CREATE TABLE t (id INTEGER, id BOOLEAN)")).columns.size() == 2,
          "Parser must leave duplicate-name validation to Binder");
    Check(std::get<CreateTableStatement>(Parser::Parse("CREATE TABLE t (s VARCHAR(4294967295))")).columns[0].max_length == UINT32_MAX,
          "VARCHAR maximum boundary failed");
    for (const auto input : {"CREATE TABLE t ()", "CREATE TABLE t (v VARCHAR(0))", "CREATE TABLE t (v VARCHAR(-1))",
         "CREATE TABLE t (v VARCHAR(4294967296))", "CREATE TABLE t (v VARCHAR)", "CREATE TABLE t (v VARCHAR('2'))",
         "CREATE TABLE t id INTEGER)", "CREATE TABLE t (id INTEGER", "CREATE TABLE t (id)", "CREATE TABLE t (id FLOAT)",
         "CREATE TABLE t (id INTEGER,)", "CREATE TABLE t (id INTEGER) extra", "CREATE t (id INTEGER)"}) {
        Reject([&] { Parser::Parse(input); });
    }
}

void TestInsert() {
    const auto single = std::get<InsertStatement>(Parser::Parse("INSERT INTO t VALUES (1)"));
    Check(single.table_name == "t" && single.values.size() == 1 && std::get<std::int64_t>(single.values[0]) == 1,
          "Single INSERT AST wrong");
    const auto mixed = std::get<InsertStatement>(Parser::Parse("insert into Users values (-2, 'It''s OK', TRUE, false, NULL, '');"));
    Check(mixed.table_name == "Users" && mixed.values.size() == 6 && std::get<std::int64_t>(mixed.values[0]) == -2 &&
          std::get<std::string>(mixed.values[1]) == "It's OK" && std::get<bool>(mixed.values[2]) && !std::get<bool>(mixed.values[3]) &&
          std::holds_alternative<std::monostate>(mixed.values[4]) && std::get<std::string>(mixed.values[5]).empty(), "Literal AST mismatch");
    const auto limits = std::get<InsertStatement>(Parser::Parse("INSERT INTO t VALUES (-9223372036854775808, 9223372036854775807, 2147483648)"));
    Check(std::get<std::int64_t>(limits.values[0]) == std::numeric_limits<std::int64_t>::min() &&
          std::get<std::int64_t>(limits.values[1]) == std::numeric_limits<std::int64_t>::max() &&
          std::get<std::int64_t>(limits.values[2]) == 2147483648LL, "Integer literal range wrong");
    for (const auto input : {"INSERT t VALUES (1)", "INSERT INTO t (1)", "INSERT INTO t VALUES ()", "INSERT INTO t VALUES (1,)",
         "INSERT INTO t VALUES (name)", "INSERT INTO t VALUES (1 2)", "INSERT INTO t VALUES (1), (2)",
         "INSERT INTO t VALUES (9223372036854775808)", "INSERT INTO t VALUES (-9223372036854775809)",
         "INSERT INTO t VALUES (1.5)", "INSERT INTO t VALUES (1+2)", "INSERT INTO t VALUES ('a' 'b')"}) {
        Reject([&] { Parser::Parse(input); });
    }
}

void TestSelectAndErrors() {
    const auto all = std::get<SelectStatement>(Parser::Parse(" \n SeLeCt * FrOm Users; \t"));
    Check(all.select_all && all.column_names.empty() && all.table_name == "Users", "SELECT all AST wrong");
    const auto single = std::get<SelectStatement>(Parser::Parse("SELECT id FROM t"));
    Check(!single.select_all && single.column_names == std::vector<std::string>{"id"}, "Single projection AST wrong");
    const auto multi = std::get<SelectStatement>(Parser::Parse("SELECT id, Name FROM t;"));
    Check(!multi.select_all && multi.column_names == std::vector<std::string>({"id", "Name"}), "Projection order/spelling lost");
    for (const auto input : {"", " ", ";", "SELECT * t", "SELECT * FROM", "SELECT FROM t", "SELECT id, FROM t",
         "SELECT *, id FROM t", "SELECT id, * FROM t", "SELECT * FROM t WHERE", "SELECT * FROM t JOIN u",
         "SELECT * FROM t;;", "SELECT * FROM t; SELECT * FROM u", "UPDATE t", "DELETE t", "DROP t", "ALTER TABLE t"}) {
        Reject([&] { Parser::Parse(input); });
    }
    try {
        Parser::Parse("SELECT *\nFROM ;");
        throw std::runtime_error("Expected missing table error");
    } catch (const SqlError& error) {
        Check(error.Position().offset == 14 && error.Position().line == 2 && error.Position().column == 6, "Parser location incorrect");
    }
    // Deterministic malformed-input smoke test for termination and bounds safety.
    std::mt19937 random(2026);
    const std::string alphabet = "abc012-()';,* \n@";
    for (int i = 0; i < 500; ++i) {
        std::string input;
        for (std::size_t j = 0, size = random() % 80; j < size; ++j) { input.push_back(alphabet[random() % alphabet.size()]); }
        try { Parser::Parse(input); } catch (const SqlError&) {}
    }
}

}  // namespace

int main() {
    try {
        TestLexer();
        TestCreate();
        TestInsert();
        TestSelectAndErrors();
        std::cout << "SQL tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
