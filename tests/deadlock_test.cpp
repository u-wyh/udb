#include "udb/database.h"
#include "udb/lock_manager.h"
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

void TestWaitsForGraphAndVictim() {
    LockManager locks;
    TransactionManager transactions(nullptr, nullptr, &locks);
    auto& older = transactions.Begin();
    auto& younger = transactions.Begin();
    locks.LockTable(older, LockMode::Exclusive, 1);
    locks.LockTable(younger, LockMode::Exclusive, 2);

    std::atomic<bool> older_acquired = false;
    std::atomic<bool> older_failed = false;
    std::thread older_waiter([&] {
        try {
            locks.LockTable(older, LockMode::Exclusive, 2);
            older_acquired = true;
        } catch (...) { older_failed = true; }
    });
    WaitUntil([&] {
        const auto graph = locks.GetWaitsForGraph();
        const auto found = graph.find(older.GetId());
        return found != graph.end() && found->second.count(younger.GetId()) == 1;
    }, "Wait-for graph did not record the first dependency");

    bool victim_aborted = false;
    try {
        locks.LockTable(younger, LockMode::Exclusive, 1);
    } catch (const DeadlockError&) {
        victim_aborted = true;
        transactions.Abort(younger);
    }
    older_waiter.join();
    Check(victim_aborted && younger.GetState() == TransactionState::Aborted,
          "Youngest cycle participant was not aborted");
    Check(older_acquired && !older_failed,
          "Surviving transaction did not acquire the released lock");
    Check(locks.GetWaitsForGraph().empty(), "Resolved deadlock remained in wait-for graph");
    transactions.Commit(older);
}

void TestSqlDeadlockRollback(const std::filesystem::path& path) {
    auto database = Database::Create(path, 3);
    auto& catalog = database->GetCatalog();
    SqlEngine first(catalog);
    SqlEngine second(catalog);
    first.ExecuteSQL("CREATE TABLE left_table (id INTEGER, value INTEGER)");
    first.ExecuteSQL("CREATE TABLE right_table (id INTEGER, value INTEGER)");
    first.ExecuteSQL("INSERT INTO left_table VALUES (1, 10)");
    first.ExecuteSQL("INSERT INTO right_table VALUES (1, 20)");
    const auto left_id = catalog.GetTable("left_table").GetTableId();
    const auto right_id = catalog.GetTable("right_table").GetTableId();

    const auto first_transaction = first.ExecuteSQL("BEGIN").transaction_id.value();
    const auto second_transaction = second.ExecuteSQL("BEGIN").transaction_id.value();
    first.ExecuteSQL("UPDATE left_table SET value = 11 WHERE id = 1");
    second.ExecuteSQL("UPDATE right_table SET value = 22 WHERE id = 1");

    std::atomic<bool> first_completed = false;
    std::atomic<bool> first_failed = false;
    std::thread first_waiter([&] {
        try {
            first.ExecuteSQL("UPDATE right_table SET value = 12 WHERE id = 1");
            first_completed = true;
        } catch (...) { first_failed = true; }
    });
    WaitUntil([&] {
        const auto graph = catalog.GetLockManager().GetWaitsForGraph();
        const auto found = graph.find(first_transaction);
        return found != graph.end() && found->second.count(second_transaction) == 1;
    }, "SQL lock wait was not visible in the wait-for graph");

    bool deadlock_reported = false;
    try {
        second.ExecuteSQL("UPDATE left_table SET value = 21 WHERE id = 1");
    } catch (const DeadlockError&) {
        deadlock_reported = true;
    }
    first_waiter.join();
    Check(deadlock_reported && !second.HasActiveTransaction(),
          "SQL deadlock victim did not abort its explicit transaction");
    Check(first_completed && !first_failed, "SQL deadlock survivor did not continue");
    first.ExecuteSQL("COMMIT");

    SqlEngine reader(catalog);
    const auto left = reader.ExecuteSQL("SELECT value FROM left_table WHERE id = 1").rows;
    const auto right = reader.ExecuteSQL("SELECT value FROM right_table WHERE id = 1").rows;
    Check(left.size() == 1 && left[0].GetValue(0) == Value::Integer(11),
          "Deadlock victim rollback changed the survivor's table");
    Check(right.size() == 1 && right[0].GetValue(0) == Value::Integer(12),
          "Deadlock rollback did not restore before releasing the victim lock");
    Check(catalog.GetLockManager().GetWaitsForGraph().empty(),
          "SQL deadlock left stale wait-for edges");
    Check(catalog.GetTable(left_id).GetTableId() == left_id &&
          catalog.GetTable(right_id).GetTableId() == right_id,
          "Deadlock handling damaged catalog state");
    bool abort_logged = false;
    for (const auto& record : database->GetLogManager().GetRecords()) {
        if (record.GetTransactionId() == second_transaction &&
            record.GetType() == LogRecordType::Abort) {
            abort_logged = true;
        }
    }
    Check(abort_logged, "Deadlock victim did not use the WAL abort path");
    database->Close();
}

}  // namespace

int main() {
    try {
        TestWaitsForGraphAndVictim();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-deadlock-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSqlDeadlockRollback(directory / "database.udb");
        std::cout << "Deadlock tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
