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

void CheckIds(const ExecutionResult& result, std::initializer_list<std::int32_t> ids) {
    Check(result.rows.size() == ids.size(), "LIMIT returned the wrong row count");
    std::size_t index = 0;
    for (const auto id : ids) {
        Check(result.rows[index++].GetValue(0) == Value::Integer(id),
              "LIMIT returned the wrong row");
    }
}

void TestSyntaxBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(
        Parser::Parse("SeLeCt id FrOm t WhErE id >= 2 LiMiT 3;"));
    Check(ast.limit == 3 && ast.predicate, "LIMIT AST is wrong");
    const auto zero = std::get<SelectStatement>(Parser::Parse("SELECT * FROM t LIMIT 0"));
    Check(zero.limit == 0, "LIMIT zero was not parsed");
    for (const auto sql : {"SELECT * FROM t LIMIT", "SELECT * FROM t LIMIT NULL",
                           "SELECT * FROM t LIMIT '1'", "SELECT * FROM t LIMIT -1",
                           "SELECT * FROM t LIMIT 1 LIMIT 2", "SELECT * FROM t LIMIT 1 WHERE id = 1",
                           "SELECT * FROM t LIMIT 9223372036854775808"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER), Column("group_id", TypeId::INTEGER)});
    const auto table_id = catalog.CreateTable("t", schema).GetTableId();
    catalog.CreateIndex("idx_id", table_id, 0);
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.limit == 3, "Binder lost LIMIT");
    const auto range_node = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& range = dynamic_cast<const IndexRangeScanPlan&>(*range_node);
    Check(range.GetLimit() == 3, "IndexRangeScan plan lost LIMIT");

    const auto exact = Planner::Plan(
        Binder(catalog).Bind(Parser::Parse("SELECT id FROM t WHERE id = 1 LIMIT 0")), catalog);
    Check(dynamic_cast<const IndexScanPlan&>(*exact).GetLimit() == 0,
          "IndexScan plan lost LIMIT zero");
    const auto sequential = Planner::Plan(
        Binder(catalog).Bind(Parser::Parse("SELECT id FROM t WHERE group_id = 1 LIMIT 2")), catalog);
    Check(dynamic_cast<const SeqScanPlan&>(*sequential).GetLimit() == 2,
          "SeqScan plan lost LIMIT");
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, big BIGINT)");
        for (int i = 0; i < 10; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i % 2) + ", " +
                              std::to_string(3000000000LL + i) + ")");
        }
        CheckIds(engine.ExecuteSQL("SELECT id FROM t LIMIT 3"), {0, 1, 2});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE group_id = 1 LIMIT 2"), {1, 3});
        Check(engine.ExecuteSQL("SELECT id FROM t WHERE group_id = 1 LIMIT 0").rows.empty(),
              "SeqScan LIMIT zero returned rows");
        CheckIds(engine.ExecuteSQL("SELECT id FROM t LIMIT 100"), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        engine.ExecuteSQL("CREATE INDEX idx_big ON t(big)");
        auto result = engine.ExecuteSQL("SELECT id FROM t WHERE id = 5 LIMIT 0");
        Check(result.type == PlanType::IndexScan && result.rows.empty(),
              "IndexScan LIMIT zero returned rows");
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id = 5 LIMIT 2");
        Check(result.type == PlanType::IndexScan, "Exact equality stopped using IndexScan");
        CheckIds(result, {5});
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 2 LIMIT 3");
        Check(result.type == PlanType::IndexRangeScan, "Range LIMIT stopped using IndexRangeScan");
        CheckIds(result, {2, 3, 4});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE id >= 0 AND group_id = 1 LIMIT 3"),
                 {1, 3, 5});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE big >= 3000000007 LIMIT 2"), {7, 8});

        engine.ExecuteSQL("UPDATE t SET id = 20 WHERE id = 9");
        engine.ExecuteSQL("DELETE FROM t WHERE id = 0");
        engine.ExecuteSQL("INSERT INTO t VALUES (10, 0, 4000000000)");
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE id >= 8 LIMIT 3"), {8, 10, 20});
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 8 LIMIT 2");
        Check(result.type == PlanType::IndexRangeScan, "Persistent LIMIT query lost range scan");
        CheckIds(result, {8, 10});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t LIMIT 2"), {1, 2});
        database->Close();
    }
    {
        auto database = Database::Create(path.parent_path() / "empty.udb", 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER)");
        Check(engine.ExecuteSQL("SELECT * FROM t LIMIT 5").rows.empty(),
              "LIMIT on an empty table returned rows");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-limit-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSyntaxBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "LIMIT tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
