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

void TestBindingAndPlanning(const std::filesystem::path& path) {
    const auto ast = std::get<SelectStatement>(Parser::Parse(
        "SELECT department, COUNT(*), SUM(amount) FROM t WHERE amount > 0 GROUP BY department"));
    Check(ast.group_by == "department" && ast.column_names.size() == 1 &&
          ast.aggregates.size() == 2, "GROUP BY AST is wrong");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    catalog.CreateTable("t", Schema({Column("department", TypeId::VARCHAR, 20),
        Column("amount", TypeId::INTEGER), Column("other", TypeId::INTEGER)}));
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.group_by_column == 0 && bound.project_group_by &&
          bound.output_schema.GetColumnCount() == 3 &&
          bound.output_schema.GetColumn(0).GetName() == "department",
          "GROUP BY binding is wrong");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& aggregate = dynamic_cast<const AggregatePlan&>(*plan);
    Check(aggregate.GetGroupByColumn() == 0 && aggregate.ProjectsGroupBy(),
          "GROUP BY plan is wrong");

    const auto hidden = std::get<BoundSelectStatement>(Binder(catalog).Bind(
        Parser::Parse("SELECT COUNT(*) FROM t GROUP BY department")));
    Check(hidden.group_by_column == 0 && !hidden.project_group_by &&
          hidden.output_schema.GetColumnCount() == 1, "hidden group key binding is wrong");
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT department FROM t GROUP BY department")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT other, COUNT(*) FROM t GROUP BY department")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT department, other, COUNT(*) FROM t GROUP BY department")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT department, COUNT(*) FROM t GROUP BY missing")); });
    Reject<SqlError>([&] { Parser::Parse("SELECT department, COUNT(*) FROM t GROUP BY department, other"); });
}

void CheckGroups(const ExecutionResult& result) {
    Check(result.type == PlanType::Aggregate && result.rows.size() == 3,
          "GROUP BY result shape is wrong");
    const auto& a = result.rows[0];
    Check(a.GetValue(0) == Value::Varchar("a") && a.GetValue(1) == Value::BigInt(2) &&
          a.GetValue(2) == Value::BigInt(2) && a.GetValue(3) == Value::BigInt(30) &&
          std::abs(a.GetValue(4).GetDouble() - 15.0) < 1e-12, "group a is wrong");
    const auto& b = result.rows[1];
    Check(b.GetValue(0) == Value::Varchar("b") && b.GetValue(1) == Value::BigInt(2) &&
          b.GetValue(2) == Value::BigInt(1) && b.GetValue(3) == Value::BigInt(5) &&
          std::abs(b.GetValue(4).GetDouble() - 5.0) < 1e-12, "group b is wrong");
    const auto& null_group = result.rows[2];
    Check(null_group.GetValue(0).IsNull() && null_group.GetValue(1) == Value::BigInt(1) &&
          null_group.GetValue(3) == Value::BigInt(7), "NULL group is wrong");
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (department VARCHAR(20), amount INTEGER)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('a', 10)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('a', 20)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('b', 5)");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, 7)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('b', NULL)");
        CheckGroups(engine.ExecuteSQL(
            "SELECT department, COUNT(*), COUNT(amount), SUM(amount), AVG(amount) FROM t GROUP BY department"));

        const auto filtered = engine.ExecuteSQL(
            "SELECT department, COUNT(*) FROM t WHERE amount > 6 GROUP BY department");
        Check(filtered.rows.size() == 2 && filtered.rows[0].GetValue(0) == Value::Varchar("a") &&
              filtered.rows[0].GetValue(1) == Value::BigInt(2) &&
              filtered.rows[1].GetValue(0).IsNull(), "filtered GROUP BY is wrong");
        const auto paged = engine.ExecuteSQL(
            "SELECT department, COUNT(*) FROM t GROUP BY department LIMIT 1 OFFSET 1");
        Check(paged.rows.size() == 1 && paged.rows[0].GetValue(0) == Value::Varchar("b"),
              "GROUP BY pagination is wrong");
        const auto hidden = engine.ExecuteSQL("SELECT COUNT(*) FROM t GROUP BY department");
        Check(hidden.rows.size() == 3 && hidden.rows[0].GetValue(0) == Value::BigInt(2) &&
              hidden.rows[2].GetValue(0) == Value::BigInt(1), "hidden group key result is wrong");

        engine.ExecuteSQL("CREATE TABLE empty (department VARCHAR(20), amount INTEGER)");
        Check(engine.ExecuteSQL(
            "SELECT department, COUNT(*) FROM empty GROUP BY department").rows.empty(),
            "empty GROUP BY returned a group");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        CheckGroups(engine.ExecuteSQL(
            "SELECT department, COUNT(*), COUNT(amount), SUM(amount), AVG(amount) FROM t GROUP BY department"));
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-group-by-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "GROUP BY tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
