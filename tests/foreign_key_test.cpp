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

void VerifyMetadata(Catalog& catalog) {
    const auto& parent = catalog.GetTable("parents");
    const auto& child = catalog.GetTable("children");
    const auto& foreign_keys = child.GetSchema().GetForeignKeys();
    Check(foreign_keys.size() == 1 && foreign_keys[0].column_indexes == std::vector<std::size_t>{1} &&
              foreign_keys[0].referenced_table_id == parent.GetTableId() &&
              foreign_keys[0].referenced_column_indexes == std::vector<std::size_t>{0},
          "FOREIGN KEY metadata is incorrect");
}

void TestForeignKeys(const std::filesystem::path& path) {
    const auto parsed = std::get<CreateTableStatement>(Parser::Parse(
        "CREATE TABLE c (id INTEGER, parent_id INTEGER, "
        "FOREIGN KEY (parent_id) REFERENCES p(id))"));
    Check(parsed.foreign_keys.size() == 1 && parsed.foreign_keys[0].column_names[0] == "parent_id" &&
              parsed.foreign_keys[0].referenced_table_name == "p" &&
              parsed.foreign_keys[0].referenced_column_names[0] == "id",
          "FOREIGN KEY syntax was not parsed");

    {
        auto database = Database::Create(path, 3);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        sql.ExecuteSQL("CREATE TABLE parents (id INTEGER PRIMARY KEY, code VARCHAR(8) UNIQUE, note VARCHAR(8))");
        sql.ExecuteSQL("CREATE TABLE children (id INTEGER PRIMARY KEY, parent_id INTEGER, "
                       "FOREIGN KEY (parent_id) REFERENCES parents(id))");
        VerifyMetadata(catalog);

        Reject([&] { sql.ExecuteSQL("CREATE TABLE missing (v INTEGER, FOREIGN KEY (v) REFERENCES absent(id))"); });
        Reject([&] { sql.ExecuteSQL("CREATE TABLE nonunique (v VARCHAR(8), FOREIGN KEY (v) REFERENCES parents(note))"); });
        Reject([&] { sql.ExecuteSQL("CREATE TABLE mismatch (v BIGINT, FOREIGN KEY (v) REFERENCES parents(id))"); });

        sql.ExecuteSQL("INSERT INTO parents VALUES (1, 'one', 'note')");
        sql.ExecuteSQL("INSERT INTO children VALUES (10, 1)");
        sql.ExecuteSQL("INSERT INTO children VALUES (11, NULL)");
        Reject([&] { sql.ExecuteSQL("INSERT INTO children VALUES (12, 99)"); });
        Reject([&] { sql.ExecuteSQL("UPDATE children SET parent_id = 99 WHERE id = 10"); });
        Check(sql.ExecuteSQL("SELECT parent_id FROM children WHERE id = 10").rows[0]
                  .GetValue(0).GetInteger() == 1,
              "Rejected child UPDATE changed data");
        Reject([&] { sql.ExecuteSQL("DELETE FROM parents WHERE id = 1"); });
        Reject([&] { sql.ExecuteSQL("UPDATE parents SET id = 2 WHERE id = 1"); });
        Reject([&] { sql.ExecuteSQL("DROP TABLE parents"); });

        sql.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        sql.ExecuteSQL("INSERT INTO parents VALUES (2, 'two', 'note2')");
        sql.ExecuteSQL("INSERT INTO children VALUES (12, 2)");
        sql.ExecuteSQL("ROLLBACK");
        Check(sql.ExecuteSQL("SELECT * FROM parents WHERE id = 2").rows.empty() &&
                  sql.ExecuteSQL("SELECT * FROM children WHERE id = 12").rows.empty(),
              "Transactional FOREIGN KEY fixture did not roll back");

        sql.ExecuteSQL("DELETE FROM children WHERE id = 10");
        sql.ExecuteSQL("UPDATE parents SET id = 2 WHERE id = 1");
        sql.ExecuteSQL("INSERT INTO children VALUES (12, 2)");
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        VerifyMetadata(catalog);
        Check(sql.ExecuteSQL("SELECT * FROM children WHERE parent_id = 2").rows.size() == 1,
              "Reopened FOREIGN KEY data is missing");
        Reject([&] { sql.ExecuteSQL("INSERT INTO children VALUES (13, 3)"); });
        Reject([&] { sql.ExecuteSQL("DELETE FROM parents WHERE id = 2"); });
        sql.ExecuteSQL("DELETE FROM children WHERE id = 12");
        sql.ExecuteSQL("DELETE FROM parents WHERE id = 2");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-foreign-key-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestForeignKeys(directory / "foreign-key.udb");
        std::cout << "Foreign key tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
