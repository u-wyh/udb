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

void TestRepeatableRead(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");

    SqlEngine reader(catalog);
    const auto reader_id = *reader.ExecuteSQL(
        "BEGIN ISOLATION LEVEL REPEATABLE READ").transaction_id;
    Check(reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows[0].GetValue(0) ==
              Value::Integer(10), "Initial repeatable snapshot is wrong");

    SqlEngine writer(catalog);
    writer.ExecuteSQL("UPDATE t SET value = 20 WHERE id = 1");
    writer.ExecuteSQL("INSERT INTO t VALUES (2, 30)");
    Check(reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows[0].GetValue(0) ==
              Value::Integer(10), "Repeatable read observed a later update");
    Check(reader.ExecuteSQL("SELECT * FROM t WHERE id >= 1 ORDER BY id").rows.size() == 1,
          "Repeatable read observed a phantom insert");
    Check(catalog.GetTransactionManager().GetTransaction(reader_id).GetSharedTableLocks().empty(),
          "MVCC repeatable read retained a shared table lock");

    bool conflict = false;
    try { reader.ExecuteSQL("UPDATE t SET value = 40 WHERE id = 1"); }
    catch (const WriteConflictError&) { conflict = true; }
    Check(conflict && !reader.HasActiveTransaction(),
          "Repeatable read did not reject a stale-snapshot write");
    Check(setup.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows[0].GetValue(0) ==
              Value::Integer(20), "Write conflict changed the committed tuple");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-rr-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 2);
        TestRepeatableRead(database->GetCatalog());
        database->Close();
        std::cout << "MVCC REPEATABLE READ tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
