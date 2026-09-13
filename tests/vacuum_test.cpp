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

void TestVacuum(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value VARCHAR(30))");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 'one')");
    setup.ExecuteSQL("INSERT INTO t VALUES (2, 'two')");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
    const auto deleted_rid = *catalog.GetIndex("id_idx").GetTree().GetValue(2);

    SqlEngine old(catalog);
    old.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    setup.ExecuteSQL("UPDATE t SET id = 10, value = 'new' WHERE id = 1");
    setup.ExecuteSQL("DELETE FROM t WHERE id = 2");
    Check(catalog.Vacuum() == 0,
          "Vacuum reclaimed a tombstone still needed by the watermark");
    Check(old.ExecuteSQL("SELECT * FROM t WHERE id = 1").rows.size() == 1 &&
              old.ExecuteSQL("SELECT * FROM t WHERE id = 2").rows.size() == 1,
          "Vacuum removed a version visible to the oldest snapshot");
    Check(!catalog.GetTransactionManager().GetStaleIndexEntries(
               catalog.GetIndex("id_idx").GetMetadata().GetIndexId()).empty(),
          "Vacuum removed stale entries before the watermark advanced");

    old.ExecuteSQL("COMMIT");
    Check(catalog.Vacuum() == 1, "Vacuum did not physically reclaim a safe tombstone");
    Check(!catalog.GetTransactionManager().GetVersionLink(deleted_rid) &&
              catalog.GetTransactionManager().GetStaleIndexEntries(
                  catalog.GetIndex("id_idx").GetMetadata().GetIndexId()).empty(),
          "Vacuum retained unreachable undo or stale index state");
    bool deleted = false;
    try { static_cast<void>(catalog.GetTableHeap("t").GetRecord(deleted_rid)); }
    catch (const std::out_of_range&) { deleted = true; }
    Check(deleted, "Vacuum left a physical tombstone occupied");

    const auto inserted = setup.ExecuteSQL("INSERT INTO t VALUES (20, 'later')").inserted_rid;
    Check(inserted && *inserted != deleted_rid,
          "A physically deleted RID was incorrectly reused");
    Check(setup.ExecuteSQL("SELECT id FROM t ORDER BY id").rows.size() == 2,
          "Table contents are wrong after vacuum and reinsertion");
    catalog.GetIndex("id_idx").GetTree().Validate();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-vacuum-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 2);
        TestVacuum(database->GetCatalog());
        database->Close();
        std::cout << "Vacuum and watermark tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
