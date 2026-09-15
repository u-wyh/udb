#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected operation to fail");
}

void CheckTree(Catalog& catalog, const std::string& table) {
    for (const auto index : catalog.GetTableIndexes(catalog.GetTable(table).GetTableId())) {
        catalog.GetIndex(index).GetTree().Validate();
    }
}

void CreateFixture(SqlEngine& sql) {
    sql.ExecuteSQL("CREATE TABLE parents (id INTEGER PRIMARY KEY)");
    sql.ExecuteSQL("CREATE TABLE children (id INTEGER PRIMARY KEY, parent_id INTEGER UNIQUE, "
                   "FOREIGN KEY (parent_id) REFERENCES parents(id) "
                   "ON DELETE CASCADE ON UPDATE CASCADE)");
    sql.ExecuteSQL("CREATE TABLE grandchildren (id INTEGER PRIMARY KEY, child_id INTEGER, "
                   "FOREIGN KEY (child_id) REFERENCES children(id) ON DELETE CASCADE)");
    sql.ExecuteSQL("CREATE TABLE nullable_children (id INTEGER PRIMARY KEY, parent_id INTEGER, "
                   "FOREIGN KEY (parent_id) REFERENCES parents(id) "
                   "ON DELETE SET NULL ON UPDATE SET NULL)");
}

void TestActions(const std::filesystem::path& path) {
    const auto parsed = std::get<CreateTableStatement>(Parser::Parse(
        "CREATE TABLE c (p INTEGER, FOREIGN KEY (p) REFERENCES x(id) "
        "ON UPDATE CASCADE ON DELETE SET NULL)"));
    Check(parsed.foreign_keys[0].on_update == ForeignKeyAction::Cascade &&
              parsed.foreign_keys[0].on_delete == ForeignKeyAction::SetNull,
          "Referential actions were not parsed");
    Reject([&] { Parser::Parse("CREATE TABLE c (p INTEGER, FOREIGN KEY (p) REFERENCES x(id) ON DELETE CASCADE ON DELETE RESTRICT)"); });

    {
        auto database = Database::Create(path, 4);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        CreateFixture(sql);
        const auto& actions = catalog.GetTable("children").GetSchema().GetForeignKeys()[0];
        Check(actions.on_delete == ForeignKeyAction::Cascade &&
                  actions.on_update == ForeignKeyAction::Cascade,
              "CASCADE metadata is incorrect");
        Reject([&] { sql.ExecuteSQL("CREATE TABLE bad (id INTEGER NOT NULL, FOREIGN KEY (id) REFERENCES parents(id) ON DELETE SET NULL)"); });

        sql.ExecuteSQL("INSERT INTO parents VALUES (1)");
        sql.ExecuteSQL("INSERT INTO children VALUES (10, 1)");
        sql.ExecuteSQL("INSERT INTO grandchildren VALUES (100, 10)");
        sql.ExecuteSQL("INSERT INTO nullable_children VALUES (20, 1)");
        sql.ExecuteSQL("UPDATE parents SET id = 2 WHERE id = 1");
        Check(sql.ExecuteSQL("SELECT * FROM children WHERE parent_id = 2").rows.size() == 1,
              "ON UPDATE CASCADE did not update the child");
        const auto nullable = sql.ExecuteSQL("SELECT parent_id FROM nullable_children WHERE id = 20");
        Check(nullable.rows.size() == 1 && nullable.rows[0].GetValue(0).IsNull(),
              "ON UPDATE SET NULL did not clear the child key");
        CheckTree(catalog, "parents");
        CheckTree(catalog, "children");

        sql.ExecuteSQL("DELETE FROM parents WHERE id = 2");
        Check(sql.ExecuteSQL("SELECT * FROM children").rows.empty() &&
                  sql.ExecuteSQL("SELECT * FROM grandchildren").rows.empty(),
              "ON DELETE CASCADE chain did not remove descendants");

        sql.ExecuteSQL("INSERT INTO parents VALUES (3)");
        sql.ExecuteSQL("INSERT INTO children VALUES (30, 3)");
        sql.ExecuteSQL("INSERT INTO grandchildren VALUES (300, 30)");
        sql.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        sql.ExecuteSQL("DELETE FROM parents WHERE id = 3");
        sql.ExecuteSQL("ROLLBACK");
        Check(sql.ExecuteSQL("SELECT * FROM parents WHERE id = 3").rows.size() == 1 &&
                  sql.ExecuteSQL("SELECT * FROM children WHERE id = 30").rows.size() == 1 &&
                  sql.ExecuteSQL("SELECT * FROM grandchildren WHERE id = 300").rows.size() == 1,
              "Rollback did not restore a cascade chain");
        database->Close();
    }
    {
        auto database = Database::Open(path, 3);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        const auto& action = catalog.GetTable("children").GetSchema().GetForeignKeys()[0];
        Check(action.on_delete == ForeignKeyAction::Cascade &&
                  action.on_update == ForeignKeyAction::Cascade,
              "Referential actions were not restored");
        sql.ExecuteSQL("DELETE FROM parents WHERE id = 3");
        Check(sql.ExecuteSQL("SELECT * FROM children").rows.empty() &&
                  sql.ExecuteSQL("SELECT * FROM grandchildren").rows.empty(),
              "Reopened cascade did not execute");
        CheckTree(catalog, "children");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-foreign-key-action-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestActions(directory / "actions.udb");
        std::cout << "Foreign key action tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
