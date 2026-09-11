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

void TestParser() {
    const auto drop = std::get<DropTableStatement>(Parser::Parse("DrOp TaBlE Users;"));
    Check(drop.table_name == "Users", "DROP AST lost table spelling");
    for (const auto sql : {"DROP", "DROP users", "DROP TABLE", "DROP TABLE users extra",
                           "DROP TABLE users;;", "DROP TABLE IF EXISTS users"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }
}

void TestCatalog(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER)});
    const auto& users = catalog.CreateTable("users", schema);
    const auto users_id = users.GetTableId();
    const auto users_page = users.GetFirstPageId();
    const auto user_rid = catalog.GetTableHeap(users_id).InsertRecord(
        Tuple(schema, {Value::Integer(7)}).Serialize(schema));
    const auto& other = catalog.CreateTable("other", schema);
    const auto other_id = other.GetTableId();
    const auto other_rid = catalog.GetTableHeap(other_id).InsertRecord(
        Tuple(schema, {Value::Integer(9)}).Serialize(schema));
    const auto size_before = std::filesystem::file_size(path);

    catalog.DropTable(users_id);
    Check(catalog.ListTables() == std::vector<table_id_t>{other_id} &&
          std::filesystem::file_size(path) == size_before, "Catalog DROP changed pages or table list incorrectly");
    Reject<std::out_of_range>([&] { catalog.GetTable(users_id); });
    Reject<std::out_of_range>([&] { catalog.GetTable("users"); });
    Reject<std::out_of_range>([&] { catalog.GetTableHeap(users_id); });
    Reject<std::out_of_range>([&] { catalog.DropTable(users_id); });
    Check(Tuple::Deserialize(catalog.GetTableHeap(other_id).GetRecord(other_rid), schema).GetValue(0) == Value::Integer(9),
          "DROP affected another table");

    const auto& recreated = catalog.CreateTable("users", schema);
    Check(recreated.GetTableId() > other_id && recreated.GetTableId() != users_id &&
          recreated.GetFirstPageId() == users_page && !catalog.GetTableHeap("users").GetFirstRID(),
          "Catalog did not reuse the freed page or recreated a nonempty table");
    Reject<std::out_of_range>([&] { catalog.GetTableHeap("users").GetRecord(user_rid); });
}

void TestBindingAndPlanning(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const auto& table = catalog.CreateTable("Users", Schema({Column("id", TypeId::INTEGER)}));
    const auto tables = catalog.ListTables();
    const Binder binder(catalog);
    const auto bound = std::get<BoundDropTableStatement>(binder.Bind(Parser::Parse("DROP TABLE Users")));
    Check(bound.table_id == table.GetTableId() && bound.table_name == "Users", "DROP binding wrong");
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DROP TABLE users")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DROP TABLE missing")); });
    const auto plan_node = Planner::Plan(BoundStatement{bound});
    const auto& plan = dynamic_cast<const DropTablePlan&>(*plan_node);
    Check(plan.GetType() == PlanType::DropTable && plan.GetTableId() == table.GetTableId() &&
          plan.GetOutputSchema().GetColumnCount() == 0, "DROP plan wrong");
    Check(catalog.ListTables() == tables && catalog.GetTable("Users").GetTableId() == table.GetTableId(),
          "Binder or Planner executed DROP");
}

void TestSqlAndPersistence(const std::filesystem::path& path) {
    table_id_t old_users_id;
    table_id_t new_users_id;
    table_id_t other_id;
    table_id_t gone_id;
    page_id_t old_users_page;
    std::uintmax_t size_after_drop;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE users (id INTEGER, name VARCHAR(2000))");
        engine.ExecuteSQL("CREATE TABLE other (id INTEGER)");
        engine.ExecuteSQL("CREATE TABLE gone (id INTEGER)");
        old_users_id = catalog.GetTable("users").GetTableId();
        other_id = catalog.GetTable("other").GetTableId();
        gone_id = catalog.GetTable("gone").GetTableId();
        old_users_page = catalog.GetTable("users").GetFirstPageId();
        for (int i = 0; i < 6; ++i) {
            engine.ExecuteSQL("INSERT INTO users VALUES (" + std::to_string(i) +
                ", '" + std::string(1400, static_cast<char>('a' + i)) + "')");
        }
        engine.ExecuteSQL("INSERT INTO other VALUES (42)");
        engine.ExecuteSQL("INSERT INTO gone VALUES (8)");
        const auto size_before = std::filesystem::file_size(path);

        const auto dropped = engine.ExecuteSQL("DROP TABLE users");
        Check(dropped.type == PlanType::DropTable && dropped.affected_rows == 0 && dropped.rows.empty(),
              "DROP execution result wrong");
        Check(std::filesystem::file_size(path) == size_before, "DROP truncated data file");
        Reject<BindError>([&] { engine.ExecuteSQL("SELECT * FROM users"); });
        Reject<BindError>([&] { engine.ExecuteSQL("INSERT INTO users VALUES (1, 'x')"); });
        Reject<BindError>([&] { engine.ExecuteSQL("UPDATE users SET id = 1"); });
        Reject<BindError>([&] { engine.ExecuteSQL("DELETE FROM users"); });
        Reject<BindError>([&] { engine.ExecuteSQL("DROP TABLE users"); });
        Check(engine.ExecuteSQL("SELECT * FROM other").rows.at(0).GetValue(0) == Value::Integer(42),
              "DROP affected other table data");

        engine.ExecuteSQL("CREATE TABLE users (id INTEGER, name VARCHAR(20))");
        new_users_id = catalog.GetTable("users").GetTableId();
        Check(new_users_id > gone_id && new_users_id != old_users_id &&
              catalog.GetTable("users").GetFirstPageId() == old_users_page &&
              engine.ExecuteSQL("SELECT * FROM users").rows.empty(), "Same-name recreation did not reuse a clean page");
        engine.ExecuteSQL("INSERT INTO users VALUES (99, 'new')");
        engine.ExecuteSQL("DROP TABLE gone");
        size_after_drop = std::filesystem::file_size(path);
        database->Close();
    }
    {
        Check(std::filesystem::file_size(path) == size_after_drop, "Close reclaimed dropped pages");
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Check(catalog.GetTable("users").GetTableId() == new_users_id &&
              engine.ExecuteSQL("SELECT id FROM users").rows.at(0).GetValue(0) == Value::Integer(99),
              "Recreated table did not persist");
        Check(catalog.GetTable("other").GetTableId() == other_id &&
              engine.ExecuteSQL("SELECT id FROM other").rows.at(0).GetValue(0) == Value::Integer(42),
              "Other table did not survive DROP/reopen");
        Reject<std::out_of_range>([&] { catalog.GetTable(old_users_id); });
        Reject<std::out_of_range>([&] { catalog.GetTable(gone_id); });
        Reject<BindError>([&] { engine.ExecuteSQL("SELECT * FROM gone"); });
        const auto& after = catalog.CreateTable("after", Schema({}));
        Check(after.GetTableId() > new_users_id, "Persisted next table ID was reused");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-drop-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestParser();
        TestCatalog(directory / "catalog.udb");
        TestBindingAndPlanning(directory / "bind.udb");
        TestSqlAndPersistence(directory / "engine.udb");
        std::cout << "DROP TABLE tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
