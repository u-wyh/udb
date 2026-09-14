#include "udb/database.h"
#include "udb/lock_manager.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace udb;
using namespace udb::sql;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Predicate>
void WaitUntil(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error(message); }
        std::this_thread::sleep_for(1ms);
    }
}

void ExerciseDeadlock() {
    LockManager locks;
    TransactionManager transactions(nullptr, nullptr, &locks);
    auto& older = transactions.Begin(IsolationLevel::Serializable);
    auto& younger = transactions.Begin(IsolationLevel::Serializable);
    locks.LockTable(older, LockMode::Exclusive, 1);
    locks.LockTable(younger, LockMode::Exclusive, 2);
    std::atomic<bool> survivor = false;
    std::thread waiter([&] {
        locks.LockTable(older, LockMode::Exclusive, 2);
        survivor = true;
    });
    WaitUntil([&] { return !locks.GetWaitsForGraph().empty(); },
              "Serializable deadlock wait edge was not registered");
    bool victim = false;
    try { locks.LockTable(younger, LockMode::Exclusive, 1); }
    catch (const DeadlockError&) { victim = true; transactions.Abort(younger); }
    waiter.join();
    Check(victim && survivor, "Serializable deadlock did not abort a victim");
    transactions.Commit(older);
}

void RetryUpdate(SqlEngine& sql, const std::string& statement) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        try { sql.ExecuteSQL(statement); return; }
        catch (const SerializationFailure&) {}
        catch (const WriteConflictError&) {}
    }
    throw std::runtime_error("Serializable stress update exhausted retries");
}

void ExerciseDatabase(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 8);
        auto& catalog = database->GetCatalog();
        SqlEngine setup(catalog);
        setup.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(30), value INTEGER)");
        setup.ExecuteSQL("CREATE TABLE aux (id INTEGER)");
        setup.ExecuteSQL("INSERT INTO aux VALUES (1)");
        for (int id = 0; id < 20; ++id) {
            setup.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id % 3) + ", 'n" + std::to_string(id) +
                             "', " + std::to_string(id) + ")");
        }
        setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
        setup.ExecuteSQL("CREATE INDEX group_name_idx ON t(group_id, name)");

        SqlEngine reader(catalog);
        reader.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        Check(reader.ExecuteSQL("SELECT name FROM t WHERE id = 5").rows.size() == 1 &&
                  reader.ExecuteSQL("SELECT id FROM t WHERE id >= 4 AND id <= 6").rows.size() == 3 &&
                  reader.ExecuteSQL("SELECT * FROM t WHERE value >= 0").rows.size() == 20 &&
                  reader.ExecuteSQL("SELECT value FROM t WHERE group_id = 2 AND name = 'n5'").rows.size() == 1,
              "Serializable point/range/table/composite reads failed");
        SqlEngine writer(catalog);
        writer.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        writer.ExecuteSQL("UPDATE t SET value = 500 WHERE id = 5");
        writer.ExecuteSQL("COMMIT");
        Check(reader.ExecuteSQL("SELECT value FROM t WHERE id = 5").rows[0].GetValue(0) ==
                  Value::Integer(5), "Long Serializable snapshot changed after a writer committed");
        Check(catalog.GetTransactionManager().GetRetainedSsiTransactionCount() >= 2,
              "Concurrent reader/writer SSI metadata was reclaimed early");
        reader.ExecuteSQL("COMMIT");

        for (int repeat = 0; repeat < 3; ++repeat) {
            SqlEngine first(catalog);
            SqlEngine second(catalog);
            first.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
            second.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
            first.ExecuteSQL("SELECT COUNT(*) FROM t WHERE value >= 0");
            second.ExecuteSQL("SELECT COUNT(*) FROM t WHERE value >= 0");
            first.ExecuteSQL("UPDATE t SET value = 700 WHERE id = 0");
            first.ExecuteSQL("COMMIT");
            second.ExecuteSQL("UPDATE t SET value = 800 WHERE id = 1");
            bool rejected = false;
            try { second.ExecuteSQL("COMMIT"); }
            catch (const SerializationFailure&) { rejected = true; }
            Check(rejected, "SSI allowed a write-skew dangerous structure");
        }

        SqlEngine phantom_a(catalog);
        SqlEngine phantom_b(catalog);
        phantom_a.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        phantom_b.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        Check(phantom_a.ExecuteSQL("SELECT id FROM t WHERE id >= 100").rows.empty() &&
                  phantom_b.ExecuteSQL("SELECT id FROM t WHERE id >= 100").rows.empty(),
              "Phantom fixture range was not empty");
        phantom_a.ExecuteSQL("INSERT INTO t VALUES (100, 1, 'p100', 100)");
        phantom_a.ExecuteSQL("COMMIT");
        phantom_b.ExecuteSQL("INSERT INTO t VALUES (101, 1, 'p101', 101)");
        bool phantom_rejected = false;
        try { phantom_b.ExecuteSQL("COMMIT"); }
        catch (const SerializationFailure&) { phantom_rejected = true; }
        Check(phantom_rejected && setup.ExecuteSQL("SELECT id FROM t WHERE id >= 100").rows.size() == 1,
              "SSI allowed a phantom serialization anomaly");

        SqlEngine conflict_a(catalog);
        SqlEngine conflict_b(catalog);
        conflict_a.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        conflict_b.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        conflict_a.ExecuteSQL("UPDATE t SET value = 202 WHERE id = 2");
        conflict_a.ExecuteSQL("COMMIT");
        bool write_conflict = false;
        try { conflict_b.ExecuteSQL("UPDATE t SET value = 222 WHERE id = 2"); }
        catch (const WriteConflictError&) { write_conflict = true; }
        Check(write_conflict && !conflict_b.HasActiveTransaction(),
              "Concurrent Serializable writers lost a write conflict");

        std::atomic<bool> failed = false;
        std::mutex failure_mutex;
        std::string failure;
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 3; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    SqlEngine sql(catalog, IsolationLevel::Serializable);
                    for (int pass = 0; pass < 12; ++pass) {
                        static_cast<void>(sql.ExecuteSQL(
                            "SELECT id FROM t WHERE id >= " + std::to_string(worker * 3) +
                            " AND id <= " + std::to_string(worker * 3 + 5)));
                        static_cast<void>(sql.ExecuteSQL(
                            "SELECT value FROM t WHERE group_id = " + std::to_string(worker % 3) +
                            " AND name = 'n" + std::to_string(worker) + "'"));
                    }
                } catch (const std::exception& error) {
                    const std::lock_guard<std::mutex> lock(failure_mutex);
                    failed = true; failure = error.what();
                }
            });
        }
        for (int worker = 0; worker < 2; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    SqlEngine sql(catalog, IsolationLevel::Serializable);
                    for (int id = worker * 5 + 5; id < worker * 5 + 10; ++id) {
                        RetryUpdate(sql, "UPDATE t SET value = " + std::to_string(1000 + id) +
                                         " WHERE id = " + std::to_string(id));
                    }
                } catch (const std::exception& error) {
                    const std::lock_guard<std::mutex> lock(failure_mutex);
                    failed = true; failure = error.what();
                }
            });
        }
        for (auto& worker : workers) { worker.join(); }
        if (failed) { throw std::runtime_error("Serializable worker failed: " + failure); }
        for (const auto index : catalog.ListIndexes()) { catalog.GetIndex(index).GetTree().Validate(); }
        static_cast<void>(catalog.Vacuum());
        database->Checkpoint();

        setup.ExecuteSQL("UPDATE t SET value = 3030 WHERE id = 3");
        SqlEngine loser(catalog);
        loser.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
        loser.ExecuteSQL("UPDATE t SET id = 404, value = 4040 WHERE id = 4");
        database->Flush();
        // Simulated crash: the loser is left active and Database::Close is not called.
    }

    for (int reopen = 0; reopen < 2; ++reopen) {
        auto database = Database::Open(path, reopen == 0 ? 2 : 1);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog, IsolationLevel::Serializable);
        Check(sql.ExecuteSQL("SELECT value FROM t WHERE id = 3").rows[0].GetValue(0) ==
                  Value::Integer(3030), "Committed Serializable write was lost in recovery");
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE id = 4").rows.size() == 1 &&
                  sql.ExecuteSQL("SELECT id FROM t WHERE id = 404").rows.empty(),
              "Recovery retained a loser Serializable write");
        Check(sql.ExecuteSQL("SELECT value FROM t WHERE group_id = 1 AND name = 'n4'").rows.size() == 1,
              "Composite index changed across recovery");
        for (const auto index : catalog.ListIndexes()) { catalog.GetIndex(index).GetTree().Validate(); }
        static_cast<void>(catalog.Vacuum());
        database->Checkpoint();
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        ExerciseDeadlock();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-serializable-stress-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        for (int round = 0; round < 2; ++round) {
            ExerciseDatabase(directory / ("round-" + std::to_string(round) + ".udb"));
        }
        std::cout << "Serializable stress tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
