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

void Verify(Catalog& catalog, SqlEngine& sql) {
    const auto& table = catalog.GetTable("users");
    const auto& schema = table.GetSchema();
    Check(schema.GetColumn(0).IsPrimaryKey() && schema.GetColumn(0).IsUnique() &&
              schema.GetColumn(0).IsNotNull(),
          "PRIMARY KEY metadata or implicit NOT NULL is wrong");
    Check(!schema.GetColumn(1).IsPrimaryKey() && schema.GetColumn(1).IsUnique() &&
              !schema.GetColumn(1).IsNotNull(),
          "UNIQUE metadata is wrong");
    const auto indexes = catalog.GetTableIndexes(table.GetTableId());
    Check(indexes.size() == 2, "Key constraints did not create one index per column");
    for (const auto id : indexes) {
        const auto& index = catalog.GetIndex(id);
        Check(index.GetTree().IsUnique() && index.GetMetadata().GetColumnIndexes().size() == 1,
              "Constraint index is not a single-column unique index");
        index.GetTree().Validate();
    }
    const auto result = sql.ExecuteSQL("SELECT name FROM users WHERE id = 1");
    Check(result.type == PlanType::IndexScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0).GetVarchar() == "a",
          "Automatic PRIMARY KEY index was not usable by the planner");
}

void TestKeyConstraints(const std::filesystem::path& path) {
    const auto ast = std::get<CreateTableStatement>(Parser::Parse(
        "CREATE TABLE parsed (id INTEGER PRIMARY KEY, code VARCHAR(8) UNIQUE)"));
    Check(ast.columns[0].primary_key && ast.columns[1].unique,
          "PRIMARY KEY or UNIQUE was not parsed");
    Reject([&] { Parser::Parse("CREATE TABLE bad (id INTEGER PRIMARY)"); });

    {
        auto database = Database::Create(path, 3);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("CREATE TABLE discarded (id INTEGER PRIMARY KEY)");
        Check(catalog.GetTableIndexes(catalog.GetTable("discarded").GetTableId()).size() == 1,
              "Transactional CREATE did not build its constraint index");
        sql.ExecuteSQL("ROLLBACK");
        Reject([&] { static_cast<void>(catalog.GetTable("discarded")); });

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL(
            "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR(10) UNIQUE, note VARCHAR(30))");
        sql.ExecuteSQL("INSERT INTO users VALUES (1, 'a', 'first')");
        sql.ExecuteSQL("COMMIT");
        Verify(catalog, sql);

        Reject([&] { sql.ExecuteSQL("INSERT INTO users VALUES (1, 'b', 'duplicate pk')"); });
        Reject([&] { sql.ExecuteSQL("INSERT INTO users VALUES (2, 'a', 'duplicate unique')"); });
        Reject([&] { sql.ExecuteSQL("INSERT INTO users VALUES (NULL, 'b', 'null pk')"); });
        sql.ExecuteSQL("INSERT INTO users VALUES (2, NULL, 'nullable unique')");
        sql.ExecuteSQL("INSERT INTO users VALUES (3, NULL, 'second null')");

        sql.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        Reject([&] { sql.ExecuteSQL("UPDATE users SET name = 'a' WHERE id = 2"); });
        Check(!sql.HasActiveTransaction(), "Unique conflict did not abort explicit transaction");
        Check(sql.ExecuteSQL("SELECT id FROM users WHERE name = 'a'").rows.size() == 1,
              "Unique conflict damaged the original row or index");

        const auto constraint_index = catalog.GetTableIndexes(
            catalog.GetTable("users").GetTableId()).front();
        Reject([&] { sql.ExecuteSQL("DROP INDEX " +
            catalog.GetIndex(constraint_index).GetMetadata().GetIndexName()); });
        Check(catalog.GetTableIndexes(catalog.GetTable("users").GetTableId()).size() == 2,
              "DROP INDEX removed a constraint backing index");
        Reject([&] { sql.ExecuteSQL("CREATE TABLE two_pk (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)"); });
        Reject([&] { sql.ExecuteSQL("CREATE TABLE bool_unique (v BOOLEAN UNIQUE)"); });
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        Verify(database->GetCatalog(), sql);
        Reject([&] { sql.ExecuteSQL("UPDATE users SET id = 1 WHERE id = 2"); });
        Check(sql.ExecuteSQL("SELECT * FROM users").rows.size() == 3,
              "Reopened key constraint failure changed table data");
        sql.ExecuteSQL("DELETE FROM users WHERE id = 1");
        sql.ExecuteSQL("INSERT INTO users VALUES (1, 'a', 'reused')");
        Verify(database->GetCatalog(), sql);
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-key-constraint-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestKeyConstraints(directory / "constraints.udb");
        std::cout << "Key constraint tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
