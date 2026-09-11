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

void CheckIds(const ExecutionResult& result, std::initializer_list<std::int32_t> ids) {
    Check(result.rows.size() == ids.size(), "multi-column ORDER BY row count is wrong");
    std::size_t position = 0;
    for (const auto id : ids) {
        Check(result.rows[position++].GetValue(0) == Value::Integer(id),
              "multi-column ORDER BY row is wrong");
    }
}

void TestBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(
        Parser::Parse("SELECT id FROM t ORDER BY department ASC, score DESC, id"));
    Check(ast.order_by.size() == 3 && ast.order_by[0].ascending &&
          !ast.order_by[1].ascending && ast.order_by[2].ascending,
          "multi-column ORDER BY AST is wrong");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("department", TypeId::VARCHAR, 20), Column("score", TypeId::INTEGER)}));
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.order_by.size() == 3 && bound.order_by[0].column_index == 1 &&
          bound.order_by[1].column_index == 2 && bound.order_by[2].column_index == 0,
          "Binder lost multi-column ORDER BY");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& scan = dynamic_cast<const SeqScanPlan&>(*plan);
    Check(scan.GetOrderBy().size() == 3 && !scan.GetOrderBy()[1].ascending,
          "Planner lost multi-column ORDER BY");
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, department VARCHAR(20), score INTEGER)");
        engine.ExecuteSQL("INSERT INTO t VALUES (1, 'a', 2)");
        engine.ExecuteSQL("INSERT INTO t VALUES (2, 'a', 2)");
        engine.ExecuteSQL("INSERT INTO t VALUES (3, 'a', 1)");
        engine.ExecuteSQL("INSERT INTO t VALUES (4, 'b', 3)");
        engine.ExecuteSQL("INSERT INTO t VALUES (5, 'b', 1)");
        engine.ExecuteSQL("INSERT INTO t VALUES (6, NULL, 9)");
        engine.ExecuteSQL("INSERT INTO t VALUES (7, 'a', NULL)");

        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY department, score DESC"),
                 {1, 2, 3, 7, 4, 5, 6});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY department DESC, score ASC"),
                 {5, 4, 3, 1, 2, 7, 6});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY department, score DESC LIMIT 3"),
                 {1, 2, 3});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t WHERE score >= 1 ORDER BY department DESC, score ASC LIMIT 4"),
                 {5, 4, 3, 1});

        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        const auto indexed = engine.ExecuteSQL(
            "SELECT id FROM t WHERE id >= 1 ORDER BY department ASC, id DESC LIMIT 4");
        Check(indexed.type == PlanType::IndexRangeScan, "multi-column sort disabled range scan");
        CheckIds(indexed, {7, 3, 2, 1});
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY department, score DESC"),
                 {1, 2, 3, 7, 4, 5, 6});
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-multi-order-by-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "Multi-column ORDER BY tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
