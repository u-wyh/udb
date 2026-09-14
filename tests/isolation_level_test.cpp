#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::int32_t ReadValue(SqlEngine& sql) {
    const auto rows = sql.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows;
    Check(rows.size() == 1, "Expected one test row");
    return rows[0].GetValue(0).GetInteger();
}

void TestTransactionLevelValue() {
    TransactionManager manager;
    auto& committed = manager.Begin(IsolationLevel::ReadCommitted);
    auto& repeatable = manager.Begin();
    Check(committed.GetIsolationLevel() == IsolationLevel::ReadCommitted,
          "Transaction did not retain READ COMMITTED");
    Check(repeatable.GetIsolationLevel() == IsolationLevel::RepeatableRead,
          "Default isolation level is not REPEATABLE READ");
    manager.Abort(committed);
    manager.Abort(repeatable);
}

void TestReadCommitted(Catalog& catalog) {
    SqlEngine reader(catalog);
    reader.ExecuteSQL("BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED");
    Check(ReadValue(reader) == 10, "READ COMMITTED initial read is wrong");

    SqlEngine writer(catalog);
    writer.ExecuteSQL("UPDATE t SET value = 20 WHERE id = 1");
    Check(ReadValue(reader) == 20,
          "READ COMMITTED did not allow a non-repeatable read after commit");
    reader.ExecuteSQL("COMMIT");
}

void TestDirtyReadPrevention(Catalog& catalog) {
    SqlEngine writer(catalog, IsolationLevel::ReadCommitted);
    SqlEngine reader(catalog, IsolationLevel::ReadCommitted);
    writer.ExecuteSQL("BEGIN");
    writer.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 1");
    const auto reader_id = reader.ExecuteSQL("BEGIN").transaction_id.value();
    Check(ReadValue(reader) == 20, "READ COMMITTED exposed a dirty value");
    Check(catalog.GetTransactionManager().GetTransaction(reader_id).GetSharedTableLocks().empty(),
          "MVCC READ COMMITTED acquired a shared table lock");
    writer.ExecuteSQL("ROLLBACK");
    Check(ReadValue(reader) == 20, "READ COMMITTED changed after writer rollback");
    reader.ExecuteSQL("COMMIT");
}

void TestRepeatableRead(Catalog& catalog) {
    SqlEngine reader(catalog);
    SqlEngine writer(catalog);
    const auto reader_id = reader.ExecuteSQL(
        "BEGIN ISOLATION LEVEL REPEATABLE READ").transaction_id.value();
    Check(ReadValue(reader) == 20, "REPEATABLE READ initial value is wrong");
    writer.ExecuteSQL("UPDATE t SET value = 30 WHERE id = 1");
    Check(ReadValue(reader) == 20,
          "REPEATABLE READ changed its fixed snapshot");
    Check(catalog.GetTransactionManager().GetTransaction(reader_id).GetSharedTableLocks().empty(),
          "MVCC REPEATABLE READ acquired a shared table lock");
    reader.ExecuteSQL("COMMIT");
    Check(ReadValue(reader) == 30, "REPEATABLE READ did not observe data after commit");
}

void TestIsolationConfiguration(Catalog& catalog) {
    SqlEngine sql(catalog);
    sql.SetDefaultIsolationLevel(IsolationLevel::ReadCommitted);
    Check(sql.GetDefaultIsolationLevel() == IsolationLevel::ReadCommitted,
          "SQL Engine default isolation was not updated");
    sql.ExecuteSQL("BEGIN");
    bool rejected = false;
    try { sql.SetDefaultIsolationLevel(IsolationLevel::RepeatableRead); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Isolation level changed during an active transaction");
    sql.ExecuteSQL("ROLLBACK");
}

}  // namespace

int main() {
    try {
        TestTransactionLevelValue();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-isolation-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 3);
        SqlEngine setup(database->GetCatalog());
        setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
        setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
        TestReadCommitted(database->GetCatalog());
        TestDirtyReadPrevention(database->GetCatalog());
        TestRepeatableRead(database->GetCatalog());
        TestIsolationConfiguration(database->GetCatalog());
        database->Close();
        std::cout << "Isolation level tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
