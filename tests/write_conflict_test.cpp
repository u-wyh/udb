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

std::int32_t ReadValue(SqlEngine& sql, std::int32_t id) {
    const auto rows = sql.ExecuteSQL(
        "SELECT value FROM t WHERE id = " + std::to_string(id)).rows;
    Check(rows.size() == 1, "Expected one row");
    return rows[0].GetValue(0).GetInteger();
}

void TestWriteConflicts(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
    setup.ExecuteSQL("INSERT INTO t VALUES (2, 20)");
    setup.ExecuteSQL("CREATE INDEX t_id ON t(id)");

    SqlEngine winner(catalog);
    SqlEngine stale(catalog);
    winner.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    stale.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    Check(ReadValue(stale, 1) == 10, "Snapshot reader could not read its initial version");
    winner.ExecuteSQL("UPDATE t SET value = 11 WHERE id = 1");
    winner.ExecuteSQL("COMMIT");
    Check(ReadValue(stale, 1) == 10, "Stale snapshot did not retain its visible version");
    bool conflicted = false;
    try { stale.ExecuteSQL("UPDATE t SET value = 12 WHERE id = 1"); }
    catch (const WriteConflictError&) { conflicted = true; }
    Check(conflicted && !stale.HasActiveTransaction() && ReadValue(setup, 1) == 11,
          "Lost-update conflict changed the committed row");

    SqlEngine aborted(catalog);
    SqlEngine follower(catalog);
    aborted.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    follower.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    aborted.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 2");
    aborted.ExecuteSQL("ROLLBACK");
    follower.ExecuteSQL("UPDATE t SET value = 21 WHERE id = 2");
    follower.ExecuteSQL("COMMIT");
    Check(ReadValue(setup, 2) == 21, "Writer could not proceed after the first writer aborted");

    SqlEngine inserter(catalog);
    SqlEngine duplicate(catalog);
    inserter.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    duplicate.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    inserter.ExecuteSQL("INSERT INTO t VALUES (3, 30)");
    inserter.ExecuteSQL("COMMIT");
    bool duplicate_rejected = false;
    try { duplicate.ExecuteSQL("INSERT INTO t VALUES (3, 300)"); }
    catch (const std::invalid_argument&) { duplicate_rejected = true; }
    const auto key_three = catalog.GetIndex("t_id").GetTree().GetValue(3);
    Check(duplicate_rejected && !duplicate.HasActiveTransaction() && key_three &&
              ReadValue(setup, 3) == 30,
          "Concurrent unique INSERT left inconsistent table or index data");

    SqlEngine key_winner(catalog);
    SqlEngine key_loser(catalog);
    key_winner.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    key_loser.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    key_winner.ExecuteSQL("UPDATE t SET id = 4 WHERE id = 1");
    key_winner.ExecuteSQL("COMMIT");
    bool update_rejected = false;
    try { key_loser.ExecuteSQL("UPDATE t SET id = 4 WHERE id = 2"); }
    catch (const std::invalid_argument&) { update_rejected = true; }
    const auto rows = setup.ExecuteSQL("SELECT id, value FROM t ORDER BY id").rows;
    Check(update_rejected && !key_loser.HasActiveTransaction() && rows.size() == 3 &&
              !catalog.GetIndex("t_id").GetTree().GetValue(1) &&
              catalog.GetIndex("t_id").GetTree().GetValue(2) &&
              catalog.GetIndex("t_id").GetTree().GetValue(3) &&
              catalog.GetIndex("t_id").GetTree().GetValue(4),
          "Concurrent unique UPDATE left inconsistent table or index data");

    SqlEngine delete_winner(catalog);
    SqlEngine delete_loser(catalog);
    delete_winner.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    delete_loser.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    delete_winner.ExecuteSQL("DELETE FROM t WHERE id = 2");
    delete_winner.ExecuteSQL("COMMIT");
    bool delete_conflicted = false;
    try { delete_loser.ExecuteSQL("DELETE FROM t WHERE id = 2"); }
    catch (const WriteConflictError&) { delete_conflicted = true; }
    Check(delete_conflicted && !delete_loser.HasActiveTransaction() &&
              setup.ExecuteSQL("SELECT * FROM t WHERE id = 2").rows.empty() &&
              !catalog.GetIndex("t_id").GetTree().GetValue(2),
          "Concurrent DELETE resurrected a committed tombstone");
    catalog.GetIndex("t_id").GetTree().Validate();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-write-conflict-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 2);
        TestWriteConflicts(database->GetCatalog());
        database->Close();
        std::cout << "Write conflict tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
