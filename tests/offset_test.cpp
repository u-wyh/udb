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
    Check(result.rows.size() == ids.size(), "OFFSET returned the wrong row count");
    std::size_t position = 0;
    for (const auto id : ids) {
        Check(result.rows[position++].GetValue(0) == Value::Integer(id),
              "OFFSET returned the wrong row");
    }
}

void TestSyntaxAndPlans(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(
        Parser::Parse("SELECT id FROM t ORDER BY id DESC LIMIT 3 OFFSET 2"));
    Check(ast.limit == 3 && ast.offset == 2, "OFFSET AST is wrong");
    for (const auto sql : {"SELECT * FROM t OFFSET 1", "SELECT * FROM t LIMIT 1 OFFSET",
                           "SELECT * FROM t LIMIT 1 OFFSET -1",
                           "SELECT * FROM t LIMIT 1 OFFSET NULL",
                           "SELECT * FROM t LIMIT 1 OFFSET 2 OFFSET 3"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const auto id = catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("other", TypeId::INTEGER)})).GetTableId();
    catalog.CreateIndex("idx_id", id, 0);
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.offset == 2 && bound.limit == 3, "Binder lost OFFSET");
    const auto range = Planner::Plan(Binder(catalog).Bind(
        Parser::Parse("SELECT id FROM t WHERE id >= 0 LIMIT 2 OFFSET 1")), catalog);
    Check(dynamic_cast<const IndexRangeScanPlan&>(*range).GetOffset() == 1,
          "IndexRangeScan lost OFFSET");
    const auto exact = Planner::Plan(Binder(catalog).Bind(
        Parser::Parse("SELECT id FROM t WHERE id = 1 LIMIT 1 OFFSET 1")), catalog);
    Check(dynamic_cast<const IndexScanPlan&>(*exact).GetOffset() == 1,
          "IndexScan lost OFFSET");
    const auto sequential = Planner::Plan(Binder(catalog).Bind(
        Parser::Parse("SELECT id FROM t WHERE other = 1 LIMIT 1 OFFSET 1")), catalog);
    Check(dynamic_cast<const SeqScanPlan&>(*sequential).GetOffset() == 1,
          "SeqScan lost OFFSET");
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, other INTEGER)");
        for (int i = 0; i < 12; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i % 2) + ")");
        }
        CheckIds(engine.ExecuteSQL("SELECT id FROM t LIMIT 3 OFFSET 2"), {2, 3, 4});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE other = 1 LIMIT 3 OFFSET 2"), {5, 7, 9});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY id DESC LIMIT 3 OFFSET 2"), {9, 8, 7});
        Check(engine.ExecuteSQL("SELECT id FROM t LIMIT 4 OFFSET 99").rows.empty(),
              "OFFSET beyond end returned rows");
        Check(engine.ExecuteSQL("SELECT id FROM t LIMIT 0 OFFSET 2").rows.empty(),
              "LIMIT zero with OFFSET returned rows");

        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        auto result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 0 LIMIT 3 OFFSET 2");
        Check(result.type == PlanType::IndexRangeScan, "OFFSET disabled range scan");
        CheckIds(result, {2, 3, 4});
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id = 5 LIMIT 1 OFFSET 1");
        Check(result.type == PlanType::IndexScan && result.rows.empty(),
              "Exact IndexScan OFFSET is wrong");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY id DESC LIMIT 2 OFFSET 3"), {8, 7});
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-offset-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSyntaxAndPlans(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "OFFSET tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
