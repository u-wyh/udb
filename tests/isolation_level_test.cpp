#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace udb;
using namespace udb::sql;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Predicate>
void WaitUntil(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error(message); }
        std::this_thread::sleep_for(1ms);
    }
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
    const auto writer_id = writer.ExecuteSQL("BEGIN").transaction_id.value();
    writer.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 1");
    const auto reader_id = reader.ExecuteSQL("BEGIN").transaction_id.value();

    std::atomic<bool> completed = false;
    std::atomic<bool> failed = false;
    std::int32_t observed = -1;
    std::thread blocked([&] {
        try {
            observed = ReadValue(reader);
            completed = true;
        } catch (...) { failed = true; }
    });
    WaitUntil([&] {
        const auto graph = catalog.GetLockManager().GetWaitsForGraph();
        const auto found = graph.find(reader_id);
        return found != graph.end() && found->second.count(writer_id) == 1;
    }, "READ COMMITTED reader did not wait for an uncommitted writer");
    Check(!completed && !failed, "READ COMMITTED exposed a dirty value");
    writer.ExecuteSQL("ROLLBACK");
    blocked.join();
    Check(completed && !failed && observed == 20,
          "READ COMMITTED did not read the rolled-back committed value");
    reader.ExecuteSQL("COMMIT");
}

void TestRepeatableRead(Catalog& catalog) {
    SqlEngine reader(catalog);
    SqlEngine writer(catalog);
    const auto reader_id = reader.ExecuteSQL(
        "BEGIN ISOLATION LEVEL REPEATABLE READ").transaction_id.value();
    Check(ReadValue(reader) == 20, "REPEATABLE READ initial value is wrong");

    std::atomic<bool> completed = false;
    std::atomic<bool> failed = false;
    std::thread blocked([&] {
        try {
            writer.ExecuteSQL("UPDATE t SET value = 30 WHERE id = 1");
            completed = true;
        } catch (...) { failed = true; }
    });
    WaitUntil([&] {
        const auto graph = catalog.GetLockManager().GetWaitsForGraph();
        const auto found = graph.begin();
        return found != graph.end() && found->second.count(reader_id) == 1;
    }, "REPEATABLE READ did not retain its shared lock");
    Check(ReadValue(reader) == 20 && !completed,
          "REPEATABLE READ changed before transaction end");
    reader.ExecuteSQL("COMMIT");
    blocked.join();
    Check(completed && !failed && ReadValue(reader) == 30,
          "REPEATABLE READ lock was not released at commit");
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
