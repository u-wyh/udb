#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void CheckValue(SqlEngine& sql, const std::string& query, std::int32_t expected,
                const char* message) {
    const auto rows = sql.ExecuteSQL(query).rows;
    Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(expected), message);
}

void TestVersionAwareIndexes(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(40), code BIGINT)");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 5, 'old', 3000000000)");
    setup.ExecuteSQL("INSERT INTO t VALUES (2, 5, 'second', NULL)");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
    setup.ExecuteSQL("CREATE INDEX name_idx ON t(name)");
    const auto table_id = catalog.GetTable("t").GetTableId();
    BPlusTreeOptions non_unique;
    non_unique.unique = false;
    catalog.CreateIndex("group_idx", table_id, 1, non_unique);
    catalog.CreateIndex("group_name_idx", table_id, std::vector<std::size_t>{1, 2});
    setup.ExecuteSQL("CREATE INDEX code_idx ON t(code)");

    SqlEngine old(catalog);
    old.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    CheckValue(old, "SELECT id FROM t WHERE id = 1", 1,
               "Initial integer index lookup failed");
    CheckValue(old, "SELECT id FROM t WHERE name = 'old'", 1,
               "Initial VARCHAR index lookup failed");
    CheckValue(old, "SELECT id FROM t WHERE group_id = 5 AND name = 'old'", 1,
               "Initial composite index lookup failed");

    SqlEngine writer(catalog);
    writer.ExecuteSQL("UPDATE t SET id = 10, group_id = 6, name = 'new', code = 3000000001 WHERE id = 1");
    CheckValue(old, "SELECT id FROM t WHERE id = 1", 1,
               "Old snapshot lost an updated INTEGER index entry");
    CheckValue(old, "SELECT id FROM t WHERE name = 'old'", 1,
               "Old snapshot lost an updated VARCHAR index entry");
    const auto group_rows = old.ExecuteSQL(
        "SELECT id FROM t WHERE group_id = 5 ORDER BY id").rows;
    Check(group_rows.size() == 2 && group_rows[0].GetValue(0) == Value::Integer(1) &&
              group_rows[1].GetValue(0) == Value::Integer(2),
          "Old snapshot lost a non-unique index entry");
    CheckValue(old, "SELECT id FROM t WHERE group_id = 5 AND name = 'old'", 1,
               "Old snapshot lost a composite index entry");
    CheckValue(old, "SELECT id FROM t WHERE code >= 3000000000 AND code <= 3000000000", 1,
               "Old snapshot lost a BIGINT range index entry");
    Check(old.ExecuteSQL("SELECT id FROM t WHERE id = 10").rows.empty(),
          "Residual validation exposed a future indexed value");
    CheckValue(setup, "SELECT id FROM t WHERE id = 10", 10,
               "Current snapshot cannot find updated index entry");

    writer.ExecuteSQL("UPDATE t SET code = NULL WHERE id = 10");
    CheckValue(old, "SELECT id FROM t WHERE code = 3000000000", 1,
               "Value-to-NULL update lost its stale index entry");
    Check(setup.ExecuteSQL("SELECT id FROM t WHERE code = 3000000001").rows.empty(),
          "Value-to-NULL update left a current index entry");
    writer.ExecuteSQL("UPDATE t SET code = 3000000002 WHERE id = 2");
    Check(old.ExecuteSQL("SELECT id FROM t WHERE code = 3000000002").rows.empty(),
          "NULL-to-value update bypassed residual visibility validation");
    CheckValue(setup, "SELECT id FROM t WHERE code = 3000000002", 2,
               "NULL-to-value update did not create a current entry");

    writer.ExecuteSQL("DELETE FROM t WHERE id = 2");
    CheckValue(old, "SELECT id FROM t WHERE id = 2", 2,
               "Old snapshot lost an index entry after DELETE");
    Check(setup.ExecuteSQL("SELECT id FROM t WHERE id = 2").rows.empty(),
          "Current snapshot found a deleted index entry");
    old.ExecuteSQL("COMMIT");

    SqlEngine rollback(catalog);
    rollback.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    rollback.ExecuteSQL("UPDATE t SET name = 'temporary' WHERE id = 10");
    rollback.ExecuteSQL("ROLLBACK");
    CheckValue(setup, "SELECT id FROM t WHERE name = 'new'", 10,
               "Rollback did not restore the live index entry");
    Check(setup.ExecuteSQL("SELECT id FROM t WHERE name = 'temporary'").rows.empty(),
          "Rollback retained an aborted index entry");

    for (const auto index_id : catalog.GetTableIndexes(table_id)) {
        catalog.GetIndex(index_id).GetTree().Validate();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-version-aware-index-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 3);
        TestVersionAwareIndexes(database->GetCatalog());
        database->Close();
        std::cout << "Version-aware index tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
