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
        "SELECT users.id, groups.label FROM users CROSS JOIN groups WHERE users.id = groups.id"));
    Check(ast.cross_join_table == "groups" && ast.column_names.size() == 2 && ast.predicate,
          "CROSS JOIN AST is wrong");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const auto users = catalog.CreateTable("users", Schema({Column("id", TypeId::INTEGER),
        Column("name", TypeId::VARCHAR, 20)})).GetTableId();
    const auto groups = catalog.CreateTable("groups", Schema({Column("id", TypeId::INTEGER),
        Column("label", TypeId::VARCHAR, 20)})).GetTableId();
    const auto bound = std::get<BoundSelectStatement>(Binder(catalog).Bind(Statement{ast}));
    Check(bound.table_id == users && bound.second_table_id == groups &&
          bound.column_indexes == std::vector<std::size_t>({0, 3}),
          "CROSS JOIN binding is wrong");
    const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
    const auto& join = dynamic_cast<const CrossJoinPlan&>(*plan);
    Check(join.GetLeftTableId() == users && join.GetRightTableId() == groups,
          "CROSS JOIN plan is wrong");

    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT id FROM users CROSS JOIN groups")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT users.missing FROM users CROSS JOIN groups")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT * FROM users CROSS JOIN users")); });
    Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse(
        "SELECT COUNT(*) FROM users CROSS JOIN groups")); });
}

void TestExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE users (id INTEGER, name VARCHAR(20))");
        engine.ExecuteSQL("CREATE TABLE groups (id INTEGER, label VARCHAR(20))");
        engine.ExecuteSQL("INSERT INTO users VALUES (1, 'alice')");
        engine.ExecuteSQL("INSERT INTO users VALUES (2, 'bob')");
        engine.ExecuteSQL("INSERT INTO groups VALUES (1, 'admin')");
        engine.ExecuteSQL("INSERT INTO groups VALUES (2, 'user')");
        engine.ExecuteSQL("INSERT INTO groups VALUES (3, 'guest')");

        auto result = engine.ExecuteSQL(
            "SELECT users.id, groups.label FROM users CROSS JOIN groups");
        Check(result.type == PlanType::CrossJoin && result.rows.size() == 6,
              "CROSS JOIN cardinality is wrong");
        Check(result.rows[0].GetValue(0) == Value::Integer(1) &&
              result.rows[0].GetValue(1) == Value::Varchar("admin") &&
              result.rows[3].GetValue(0) == Value::Integer(2) &&
              result.rows[3].GetValue(1) == Value::Varchar("admin"),
              "CROSS JOIN order is wrong");

        result = engine.ExecuteSQL(
            "SELECT users.id, groups.label FROM users CROSS JOIN groups WHERE users.id = groups.id");
        Check(result.rows.size() == 2 && result.rows[0].GetValue(1) == Value::Varchar("admin") &&
              result.rows[1].GetValue(1) == Value::Varchar("user"), "qualified join filter is wrong");
        result = engine.ExecuteSQL(
            "SELECT name, label FROM users CROSS JOIN groups ORDER BY groups.id DESC LIMIT 2 OFFSET 1");
        Check(result.rows.size() == 2 && result.rows[0].GetValue(0) == Value::Varchar("bob") &&
              result.rows[0].GetValue(1) == Value::Varchar("guest") &&
              result.rows[1].GetValue(1) == Value::Varchar("user"),
              "CROSS JOIN sort or pagination is wrong");
        result = engine.ExecuteSQL(
            "SELECT users.id + groups.id AS total FROM users CROSS JOIN groups "
            "WHERE users.id = 1 ORDER BY groups.id DESC");
        Check(result.rows.size() == 3 && result.rows[0].GetValue(0) == Value::Integer(4) &&
              result.rows[2].GetValue(0) == Value::Integer(2), "CROSS JOIN expression is wrong");
        const auto all = engine.ExecuteSQL("SELECT * FROM users CROSS JOIN groups LIMIT 1");
        Check(all.output_schema.GetColumnCount() == 4 &&
              all.output_schema.GetColumn(0).GetName() == "users.id" &&
              all.output_schema.GetColumn(2).GetName() == "groups.id", "CROSS JOIN star schema is wrong");

        engine.ExecuteSQL("CREATE TABLE empty (id INTEGER)");
        Check(engine.ExecuteSQL("SELECT * FROM users CROSS JOIN empty").rows.empty(),
              "CROSS JOIN with empty table returned rows");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto result = engine.ExecuteSQL(
            "SELECT users.id, groups.label FROM users CROSS JOIN groups WHERE users.id = groups.id");
        Check(result.rows.size() == 2, "reopen CROSS JOIN is wrong");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-cross-join-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBindingAndPlanning(directory / "planning.udb");
        TestExecution(directory / "execution.udb");
        std::cout << "CROSS JOIN tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
