#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

const LogicalExpression& Logical(const ExpressionPtr& expression) {
    return std::get<LogicalExpression>(expression->node);
}

void TestLexerAndParser() {
    Lexer lexer("= != < <= > >= AND OR NOT WHERE");
    for (const auto type : {TokenType::Equal, TokenType::NotEqual, TokenType::Less, TokenType::LessEqual,
                            TokenType::Greater, TokenType::GreaterEqual, TokenType::And, TokenType::Or,
                            TokenType::Not, TokenType::Where}) {
        Check(lexer.Next().type == type, "Expression token mismatch");
    }
    for (const auto input : {"!", "==", "=>", "=<", "<>", "&&", "||"}) {
        Reject<SqlError>([&] { Parser::Parse(std::string("SELECT * FROM t WHERE a ") + input + " 1"); });
    }

    for (const auto op : {"=", "!=", "<", "<=", ">", ">="}) {
        const auto select = std::get<SelectStatement>(Parser::Parse(
            std::string("SELECT * FROM t WHERE id ") + op + " 1"));
        Check(select.predicate && std::holds_alternative<ComparisonExpression>(select.predicate->node),
              "Comparison AST missing");
    }
    const auto precedence = std::get<SelectStatement>(Parser::Parse(
        "SELECT * FROM t WHERE a = 1 OR b = 2 AND NOT c = 3"));
    const auto& root = Logical(precedence.predicate);
    Check(root.op == LogicalOperator::Or && Logical(root.right).op == LogicalOperator::And &&
          Logical(Logical(root.right).right).op == LogicalOperator::Not, "Operator precedence wrong");
    const auto grouped = std::get<SelectStatement>(Parser::Parse(
        "SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3"));
    Check(Logical(grouped.predicate).op == LogicalOperator::And &&
          Logical(Logical(grouped.predicate).left).op == LogicalOperator::Or, "Parentheses precedence wrong");
    for (const auto input : {"SELECT * FROM t WHERE", "SELECT * FROM t WHERE ()",
                             "SELECT * FROM t WHERE id =", "SELECT * FROM t WHERE AND id = 1",
                             "SELECT * FROM t WHERE (id = 1", "SELECT * FROM t WHERE id = 1 OR"}) {
        Reject<SqlError>([&] { Parser::Parse(input); });
    }
}

void TestBindingAndEvaluation(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER), Column("big", TypeId::BIGINT),
                         Column("active", TypeId::BOOLEAN), Column("name", TypeId::VARCHAR, 20)});
    catalog.CreateTable("t", schema);
    const Binder binder(catalog);
    auto predicate = [&](const std::string& condition) {
        const auto statement = Parser::Parse("SELECT * FROM t WHERE " + condition);
        return std::get<BoundSelectStatement>(binder.Bind(statement)).predicate;
    };

    const auto integer = predicate("id = 1");
    const auto& integer_comparison = std::get<BoundComparisonExpression>(integer->node);
    Check(std::get<BoundColumnExpression>(integer_comparison.left->node).column_index == 0 &&
          integer_comparison.right->type == TypeId::INTEGER, "INTEGER binding wrong");
    const auto bigint = predicate("big = 1");
    Check(std::get<BoundComparisonExpression>(bigint->node).right->type == TypeId::BIGINT,
          "BIGINT contextual literal binding wrong");
    const auto null = predicate("NULL = id");
    const auto& null_comparison = std::get<BoundComparisonExpression>(null->node);
    Check(std::get<BoundLiteralExpression>(null_comparison.left->node).value == Value::Null(TypeId::INTEGER),
          "NULL contextual binding wrong");
    Reject<BindError>([&] { predicate("missing = 1"); });
    Reject<BindError>([&] { predicate("id = 2147483648"); });
    Reject<BindError>([&] { predicate("id = active"); });
    Reject<BindError>([&] { predicate("NULL = NULL"); });
    Reject<BindError>([&] { predicate("id AND TRUE"); });

    const Tuple tuple(schema, {Value::Integer(1), Value::BigInt(5), Value::Boolean(true), Value::Varchar("b")});
    auto evaluate = [&](const std::string& condition) { return EvaluateExpression(*predicate(condition), tuple); };
    Check(evaluate("id = 1") == Value::Boolean(true) && evaluate("name > 'a'") == Value::Boolean(true),
          "Comparison evaluation wrong");
    Check(evaluate("2147483648 > 1") == Value::Boolean(true) &&
          evaluate("1 < 2147483648") == Value::Boolean(true), "Literal comparison typing is asymmetric");
    Check(evaluate("NULL = id").IsNull(), "NULL comparison is not NULL");
    Check(evaluate("TRUE AND NULL").IsNull(), "TRUE AND NULL wrong");
    Check(evaluate("FALSE AND NULL") == Value::Boolean(false), "FALSE AND NULL wrong");
    Check(evaluate("TRUE OR NULL") == Value::Boolean(true), "TRUE OR NULL wrong");
    Check(evaluate("FALSE OR NULL").IsNull(), "FALSE OR NULL wrong");
    Check(evaluate("NOT NULL").IsNull(), "NOT NULL wrong");

    const auto bound = std::get<BoundSelectStatement>(binder.Bind(
        Parser::Parse("SELECT name FROM t WHERE id = 1")));
    const auto plan = Planner::Plan(BoundStatement{bound});
    const auto& scan = dynamic_cast<const SeqScanPlan&>(*plan);
    Check(scan.GetPredicate() &&
          std::get<BoundColumnExpression>(std::get<BoundComparisonExpression>(scan.GetPredicate()->node).left->node).column_index == 0,
          "Planner lost bound predicate");
}

void TestSqlEngine(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(2000))");
        engine.ExecuteSQL("INSERT INTO t VALUES (1, 2147483648, TRUE, 'bob')");
        engine.ExecuteSQL("INSERT INTO t VALUES (2, 2147483650, FALSE, 'alice')");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL, NULL)");
        for (int i = 0; i < 10; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(100 + i) +
                ", 2147484000, TRUE, '" + std::string(1400, static_cast<char>('k' + i)) + "')");
        }
        auto count = [&](const std::string& where) { return engine.ExecuteSQL("SELECT * FROM t WHERE " + where).rows.size(); };
        Check(count("id = 1") == 1 && count("id != 1") == 11, "Equality filtering wrong");
        Check(count("id < 2") == 1 && count("id <= 2") == 2 && count("id > 2") == 10 && count("id >= 2") == 11,
              "INTEGER ordering wrong");
        Check(count("big = 2147483648") == 1, "BIGINT filtering wrong");
        Check(count("active = TRUE") == 11, "BOOLEAN filtering wrong");
        Check(count("name < 'bob'") == 1, "VARCHAR filtering wrong");
        Check(count("id >= 1 AND active = TRUE") == 11, "AND filtering wrong");
        Check(count("id = 1 OR id = 2") == 2, "OR filtering wrong");
        Check(count("NOT active = TRUE") == 1, "NOT filtering wrong");
        Check(count("id = 1 OR id = 2 AND active = TRUE") == 1, "AND/OR precedence wrong");
        Check(count("(id = 1 OR id = 2) AND active = TRUE") == 1, "Parenthesized filtering wrong");
        Check(count("TRUE") == 13 && count("FALSE") == 0 && count("NULL") == 0, "WHERE truth handling wrong");
        Check(count("id = NULL") == 0, "NULL comparison retained rows");
        const auto projected = engine.ExecuteSQL("SELECT name, id FROM t WHERE id >= 100");
        Check(projected.rows.size() == 10 && projected.output_schema.GetColumn(0).GetName() == "name" &&
              projected.rows[0].GetValue(1) == Value::Integer(100), "Filtered projection/multi-page scan wrong");
        Check(engine.ExecuteSQL("SELECT * FROM t").rows.size() == 13, "SELECT without WHERE changed");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto restored = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 105");
        Check(restored.rows.size() == 5 && restored.rows[0].GetValue(0) == Value::Integer(105),
              "Reopen WHERE query failed");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-expression-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestLexerAndParser();
        TestBindingAndEvaluation(directory / "bind.udb");
        TestSqlEngine(directory / "engine.udb");
        std::cout << "Expression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
