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

void TestBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(Parser::Parse(
        "SELECT id AS original, id + 2 * 3 AS calculated, 'x' AS label FROM t"));
    Check(ast.projections.size() == 3 && ast.projections[0].alias == "original" &&
          ast.projections[1].alias == "calculated", "SELECT expression AST is wrong");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("big", TypeId::BIGINT), Column("name", TypeId::VARCHAR, 20)}));
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.projections.size() == 3 && bound.output_schema.GetColumn(0).GetName() == "original" &&
          bound.output_schema.GetColumn(1).GetType() == TypeId::INTEGER &&
          bound.output_schema.GetColumn(2).GetType() == TypeId::VARCHAR,
          "SELECT expression binding is wrong");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& scan = dynamic_cast<const SeqScanPlan&>(*plan);
    Check(scan.GetProjections().size() == 3 && scan.GetColumnIndexes().empty(),
          "SELECT expression plan is wrong");

    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT id + 1 FROM t")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT name + 1 AS bad FROM t")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT id AS x, big AS x FROM t")); });
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, name VARCHAR(20))");
        engine.ExecuteSQL("INSERT INTO t VALUES (-1, 3000000000, 'minus')");
        engine.ExecuteSQL("INSERT INTO t VALUES (2, 4000000000, 'two')");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL)");

        const auto result = engine.ExecuteSQL(
            "SELECT id AS original, id + 2 * 3 AS calculated, (id + 2) * 3 AS grouped, "
            "big + 1 AS next_big, 7 AS seven, 'x' AS label, TRUE AS flag, NULL AS missing "
            "FROM t ORDER BY id");
        Check(result.rows.size() == 3 && result.output_schema.GetColumnCount() == 8,
              "SELECT expression result shape is wrong");
        Check(result.rows[0].GetValue(0) == Value::Integer(-1) &&
              result.rows[0].GetValue(1) == Value::Integer(5) &&
              result.rows[0].GetValue(2) == Value::Integer(3) &&
              result.rows[0].GetValue(3) == Value::BigInt(3000000001) &&
              result.rows[0].GetValue(4) == Value::Integer(7) &&
              result.rows[0].GetValue(5) == Value::Varchar("x") &&
              result.rows[0].GetValue(6) == Value::Boolean(true) &&
              result.rows[0].GetValue(7).IsNull(), "SELECT expression values are wrong");
        Check(result.rows[2].GetValue(1).IsNull() && result.rows[2].GetValue(3).IsNull(),
              "arithmetic NULL propagation is wrong");

        const auto filtered = engine.ExecuteSQL(
            "SELECT id > 0 AS positive, id / 2 AS half FROM t WHERE id + 1 > 0 ORDER BY id");
        Check(filtered.rows.size() == 1 && filtered.rows[0].GetValue(0) == Value::Boolean(true) &&
              filtered.rows[0].GetValue(1) == Value::Integer(1), "expression filtering is wrong");
        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        const auto indexed = engine.ExecuteSQL("SELECT id + 10 AS shifted FROM t WHERE id = 2");
        Check(indexed.type == PlanType::IndexScan && indexed.rows[0].GetValue(0) == Value::Integer(12),
              "indexed expression projection is wrong");
        Reject<std::domain_error>([&] { engine.ExecuteSQL("SELECT id / 0 AS bad FROM t"); });

        engine.ExecuteSQL("INSERT INTO t VALUES (2147483647, 5000000000, 'max')");
        Reject<std::overflow_error>([&] {
            engine.ExecuteSQL("SELECT id + 1 AS bad FROM t WHERE id = 2147483647");
        });
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto result = engine.ExecuteSQL(
            "SELECT name AS label, id - 1 AS previous FROM t WHERE id = 2");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Varchar("two") &&
              result.rows[0].GetValue(1) == Value::Integer(1), "reopen expression projection is wrong");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-select-expression-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "SELECT expression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
