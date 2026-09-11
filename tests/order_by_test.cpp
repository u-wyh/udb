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

void CheckIds(const ExecutionResult& result, std::initializer_list<std::optional<std::int32_t>> ids) {
    Check(result.rows.size() == ids.size(), "ORDER BY returned the wrong row count");
    std::size_t index = 0;
    for (const auto id : ids) {
        const auto& value = result.rows[index++].GetValue(0);
        Check(id ? value == Value::Integer(*id) : value.IsNull(), "ORDER BY returned the wrong row");
    }
}

void TestSyntaxBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(
        Parser::Parse("SELECT id FROM t WHERE id > 0 ORDER BY name DESC LIMIT 2"));
    Check(ast.order_by.size() == 1 && ast.order_by[0].column_name == "name" && !ast.order_by[0].ascending,
          "ORDER BY AST is wrong");
    Check(std::get<SelectStatement>(Parser::Parse("SELECT * FROM t ORDER BY id")).order_by[0].ascending,
          "ORDER BY default direction is wrong");
    for (const auto sql : {"SELECT * FROM t ORDER id", "SELECT * FROM t ORDER BY",
                           "SELECT * FROM t ORDER BY id DESC ASC",
                           "SELECT * FROM t LIMIT 1 ORDER BY id"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    DiskManager disk(path);
    BufferPoolManager pool(disk, 2);
    Catalog catalog(pool);
    const auto id = catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("name", TypeId::VARCHAR, 20)})).GetTableId();
    catalog.CreateIndex("idx_id", id, 0);
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.order_by.size() == 1 && bound.order_by[0].column_index == 1 && !bound.order_by[0].ascending,
          "Binder lost ORDER BY");
    const auto range = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& range_scan = dynamic_cast<const IndexRangeScanPlan&>(*range);
    Check(range_scan.GetOrderBy().size() == 1 && range_scan.GetOrderBy()[0].column_index == 1 &&
          !range_scan.GetOrderBy()[0].ascending && range_scan.GetLimit() == 2,
          "Planner lost ORDER BY");
    const auto exact = Planner::Plan(Binder(catalog).Bind(
        Parser::Parse("SELECT id FROM t WHERE id = 1 ORDER BY id")), catalog);
    Check(!dynamic_cast<const IndexScanPlan&>(*exact).GetOrderBy().empty(),
          "IndexScan lost ORDER BY");
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT id FROM t ORDER BY missing")); });
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(20))");
        engine.ExecuteSQL("INSERT INTO t VALUES (3, 3000000003, TRUE, 'c')");
        engine.ExecuteSQL("INSERT INTO t VALUES (1, 3000000001, FALSE, 'a')");
        engine.ExecuteSQL("INSERT INTO t VALUES (2, 3000000002, TRUE, 'b')");
        engine.ExecuteSQL("INSERT INTO t VALUES (4, 3000000004, FALSE, 'a')");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL, NULL)");

        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY id ASC"), {1, 2, 3, 4, std::nullopt});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY id DESC"), {4, 3, 2, 1, std::nullopt});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY name"), {1, 4, 2, 3, std::nullopt});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY active DESC"), {3, 2, 1, 4, std::nullopt});
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY big DESC LIMIT 2"), {4, 3});
        Check(engine.ExecuteSQL("SELECT id FROM t ORDER BY id LIMIT 0").rows.empty(),
              "ORDER BY LIMIT zero returned rows");

        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        auto result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 1 ORDER BY id DESC LIMIT 2");
        Check(result.type == PlanType::IndexRangeScan, "ORDER BY disabled range scan");
        CheckIds(result, {4, 3});
        result = engine.ExecuteSQL("SELECT name, id FROM t WHERE id = 2 ORDER BY name DESC");
        Check(result.type == PlanType::IndexScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Varchar("b") &&
              result.rows[0].GetValue(1) == Value::Integer(2), "Indexed projection is wrong");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        CheckIds(engine.ExecuteSQL("SELECT id FROM t ORDER BY id DESC LIMIT 3"), {4, 3, 2});
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-order-by-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSyntaxBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "ORDER BY tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
