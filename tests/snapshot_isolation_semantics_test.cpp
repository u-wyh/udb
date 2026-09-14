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

std::int32_t ValueFor(SqlEngine& sql, int id) {
    const auto rows = sql.ExecuteSQL(
        "SELECT value FROM t WHERE id = " + std::to_string(id)).rows;
    Check(rows.size() == 1, "Expected one visible row");
    return rows[0].GetValue(0).GetInteger();
}

void TestVisibilityAndConflicts(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
    setup.ExecuteSQL("INSERT INTO t VALUES (2, 20)");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");

    SqlEngine old(catalog, IsolationLevel::SnapshotIsolation);
    old.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    SqlEngine writer(catalog, IsolationLevel::SnapshotIsolation);
    writer.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    writer.ExecuteSQL("UPDATE t SET value = 11 WHERE id = 1");
    Check(ValueFor(writer, 1) == 11, "Snapshot transaction cannot read its own update");
    Check(ValueFor(old, 1) == 10, "Snapshot reader observed a dirty update");
    writer.ExecuteSQL("COMMIT");
    Check(ValueFor(old, 1) == 10, "Old snapshot observed a committed later update");

    SqlEngine current(catalog, IsolationLevel::SnapshotIsolation);
    Check(ValueFor(current, 1) == 11, "New snapshot missed a committed update");
    current.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    current.ExecuteSQL("DELETE FROM t WHERE id = 2");
    Check(current.ExecuteSQL("SELECT * FROM t WHERE id = 2").rows.empty(),
          "Snapshot transaction cannot read its own delete");
    Check(ValueFor(old, 2) == 20, "Old snapshot lost a logically deleted row");
    current.ExecuteSQL("ROLLBACK");
    Check(ValueFor(setup, 2) == 20, "Delete rollback did not restore visibility");

    SqlEngine loser(catalog, IsolationLevel::SnapshotIsolation);
    loser.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    setup.ExecuteSQL("UPDATE t SET value = 12 WHERE id = 1");
    bool conflict = false;
    try { loser.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 1"); }
    catch (const WriteConflictError&) { conflict = true; }
    Check(conflict && ValueFor(setup, 1) == 12,
          "Snapshot isolation did not prevent a lost update");
    old.ExecuteSQL("COMMIT");
}

void TestAllowedWriteSkew(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE duty (id INTEGER, active INTEGER)");
    setup.ExecuteSQL("INSERT INTO duty VALUES (1, 1)");
    setup.ExecuteSQL("INSERT INTO duty VALUES (2, 1)");

    SqlEngine left(catalog, IsolationLevel::SnapshotIsolation);
    SqlEngine right(catalog, IsolationLevel::SnapshotIsolation);
    left.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    right.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    Check(left.ExecuteSQL("SELECT * FROM duty WHERE active = 1").rows.size() == 2 &&
              right.ExecuteSQL("SELECT * FROM duty WHERE active = 1").rows.size() == 2,
          "Write-skew transactions did not start from the same snapshot");
    left.ExecuteSQL("UPDATE duty SET active = 0 WHERE id = 1");
    left.ExecuteSQL("COMMIT");
    right.ExecuteSQL("UPDATE duty SET active = 0 WHERE id = 2");
    right.ExecuteSQL("COMMIT");
    Check(setup.ExecuteSQL("SELECT * FROM duty WHERE active = 1").rows.empty(),
          "Snapshot isolation unexpectedly prevented documented write skew");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-si-semantics-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 3);
        TestVisibilityAndConflicts(database->GetCatalog());
        TestAllowedWriteSkew(database->GetCatalog());
        database->Close();
        std::cout << "Snapshot isolation semantics tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
