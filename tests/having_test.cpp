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
        "SELECT department, COUNT(*), SUM(amount) FROM t GROUP BY department "
        "HAVING COUNT(*) >= 2 AND SUM(amount) > 10"));
    Check(ast.having && ast.group_by == "department", "HAVING AST is wrong");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    catalog.CreateTable("t", Schema({Column("department", TypeId::VARCHAR, 20),
        Column("amount", TypeId::INTEGER)}));
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.having && bound.having->type == TypeId::BOOLEAN, "HAVING binding is wrong");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    Check(dynamic_cast<const AggregatePlan&>(*plan).GetHaving() != nullptr, "HAVING plan is wrong");

    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT department, COUNT(*) FROM t GROUP BY department HAVING SUM(amount) > 1")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT COUNT(*) FROM t GROUP BY department HAVING department = 'a'")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("SELECT * FROM t HAVING amount > 1")); });
    Reject<SqlError>([&] { Parser::Parse(
        "SELECT COUNT(*) FROM t LIMIT 1 HAVING COUNT(*) > 0"); });
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (department VARCHAR(20), amount INTEGER)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('a', 10)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('a', 20)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('b', 5)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('b', NULL)");
        engine.ExecuteSQL("INSERT INTO t VALUES ('c', NULL)");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, 7)");

        auto result = engine.ExecuteSQL(
            "SELECT department, COUNT(*), SUM(amount) FROM t GROUP BY department "
            "HAVING COUNT(*) >= 2 AND SUM(amount) > 10");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Varchar("a") &&
              result.rows[0].GetValue(1) == Value::BigInt(2), "group HAVING is wrong");

        result = engine.ExecuteSQL(
            "SELECT department, COUNT(*), AVG(amount) FROM t GROUP BY department HAVING AVG(amount) > 10");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Varchar("a"),
              "DOUBLE HAVING is wrong");
        result = engine.ExecuteSQL(
            "SELECT department, COUNT(*) FROM t GROUP BY department HAVING department = 'b'");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Varchar("b"),
              "group-column HAVING is wrong");
        result = engine.ExecuteSQL("SELECT COUNT(*) FROM t HAVING COUNT(*) > 3");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::BigInt(6),
              "global HAVING true is wrong");
        Check(engine.ExecuteSQL("SELECT COUNT(*) FROM t HAVING COUNT(*) > 10").rows.empty(),
              "global HAVING false returned a row");
        Check(engine.ExecuteSQL(
            "SELECT department, SUM(amount) FROM t GROUP BY department HAVING SUM(amount) > 0 LIMIT 1 OFFSET 1")
              .rows[0].GetValue(0) == Value::Varchar("b"), "HAVING pagination order is wrong");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto result = engine.ExecuteSQL(
            "SELECT department, COUNT(*) FROM t GROUP BY department HAVING COUNT(*) >= 2");
        Check(result.rows.size() == 2 && result.rows[0].GetValue(0) == Value::Varchar("a") &&
              result.rows[1].GetValue(0) == Value::Varchar("b"), "reopen HAVING is wrong");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-having-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "HAVING tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
