#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <cmath>
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

void TestSyntaxBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(Parser::Parse(
        "SELECT COUNT(*), count(id), SUM(id), MIN(name), MAX(active), AVG(big) FROM t WHERE id >= 1"));
    Check(ast.aggregates.size() == 6 && !ast.aggregates[0].column_name &&
          ast.aggregates[1].column_name == "id" && ast.predicate,
          "aggregate AST is wrong");
    for (const auto sql : {"SELECT COUNT() FROM t", "SELECT SUM(*) FROM t",
                           "SELECT COUNT(*), id FROM t",
                           "SELECT mystery(id) FROM t"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("big", TypeId::BIGINT), Column("active", TypeId::BOOLEAN),
        Column("name", TypeId::VARCHAR, 20)}));
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.aggregates.size() == 6 && bound.output_schema.GetColumnCount() == 6 &&
          bound.output_schema.GetColumn(0).GetType() == TypeId::BIGINT &&
          bound.output_schema.GetColumn(5).GetType() == TypeId::DOUBLE,
          "aggregate binding is wrong");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& aggregate = dynamic_cast<const AggregatePlan&>(*plan);
    Check(aggregate.GetType() == PlanType::Aggregate && aggregate.GetAggregates().size() == 6,
          "aggregate plan is wrong");
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT SUM(name) FROM t")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT AVG(active) FROM t")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT COUNT(missing) FROM t")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT COUNT(*) FROM t ORDER BY id")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT id, COUNT(*) FROM t")); });
}

void CheckAggregateRow(const ExecutionResult& result) {
    Check(result.type == PlanType::Aggregate && result.rows.size() == 1 &&
          result.output_schema.GetColumnCount() == 8, "aggregate result shape is wrong");
    const auto& row = result.rows[0];
    Check(row.GetValue(0) == Value::BigInt(4) && row.GetValue(1) == Value::BigInt(3),
          "COUNT result is wrong");
    Check(row.GetValue(2) == Value::BigInt(6) && row.GetValue(3) == Value::BigInt(12000000000LL),
          "SUM result is wrong");
    Check(row.GetValue(4) == Value::Integer(1) && row.GetValue(5) == Value::Varchar("c") &&
          row.GetValue(6) == Value::Boolean(false), "MIN/MAX result is wrong");
    Check(std::abs(row.GetValue(7).GetDouble() - 2.0) < 1e-12, "AVG result is wrong");
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(20))");
        engine.ExecuteSQL("INSERT INTO t VALUES (1, 3000000000, TRUE, 'b')");
        engine.ExecuteSQL("INSERT INTO t VALUES (2, 4000000000, FALSE, 'a')");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL, NULL)");
        engine.ExecuteSQL("INSERT INTO t VALUES (3, 5000000000, TRUE, 'c')");
        CheckAggregateRow(engine.ExecuteSQL(
            "SELECT COUNT(*), COUNT(id), SUM(id), SUM(big), MIN(id), MAX(name), MIN(active), AVG(id) FROM t"));

        const auto filtered = engine.ExecuteSQL(
            "SELECT COUNT(*), SUM(id), AVG(id) FROM t WHERE id >= 2");
        Check(filtered.rows[0].GetValue(0) == Value::BigInt(2) &&
              filtered.rows[0].GetValue(1) == Value::BigInt(5) &&
              std::abs(filtered.rows[0].GetValue(2).GetDouble() - 2.5) < 1e-12,
              "filtered aggregate is wrong");
        Check(engine.ExecuteSQL("SELECT COUNT(*) FROM t LIMIT 0").rows.empty(),
              "aggregate LIMIT zero returned a row");
        Check(engine.ExecuteSQL("SELECT COUNT(*) FROM t LIMIT 1 OFFSET 1").rows.empty(),
              "aggregate OFFSET returned a row");

        engine.ExecuteSQL("CREATE TABLE empty (id INTEGER, name VARCHAR(20))");
        const auto empty = engine.ExecuteSQL(
            "SELECT COUNT(*), COUNT(id), SUM(id), MIN(name), MAX(id), AVG(id) FROM empty");
        Check(empty.rows.size() == 1 && empty.rows[0].GetValue(0) == Value::BigInt(0) &&
              empty.rows[0].GetValue(1) == Value::BigInt(0), "empty COUNT is wrong");
        for (std::size_t i = 2; i < 6; ++i) {
            Check(empty.rows[0].GetValue(i).IsNull(), "empty aggregate must be NULL");
        }
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        CheckAggregateRow(engine.ExecuteSQL(
            "SELECT COUNT(*), COUNT(id), SUM(id), SUM(big), MIN(id), MAX(name), MIN(active), AVG(id) FROM t"));
        database->Close();
    }
}

void TestOverflow(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    SqlEngine engine(database->GetCatalog());
    engine.ExecuteSQL("CREATE TABLE t (value BIGINT)");
    engine.ExecuteSQL("INSERT INTO t VALUES (9223372036854775807)");
    engine.ExecuteSQL("INSERT INTO t VALUES (9223372036854775807)");
    Reject<std::overflow_error>([&] { engine.ExecuteSQL("SELECT SUM(value) FROM t"); });
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-aggregate-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSyntaxBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        TestOverflow(directory / "overflow.udb");
        std::cout << "Aggregate tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
