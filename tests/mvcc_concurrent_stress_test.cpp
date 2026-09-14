#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <filesystem>
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

void Verify(Catalog& catalog) {
    SqlEngine sql(catalog, IsolationLevel::SnapshotIsolation);
    const auto rows = sql.ExecuteSQL("SELECT id, value FROM t ORDER BY id").rows;
    Check(rows.size() == 29, "Stress table row count is wrong");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto id = rows[i].GetValue(0).GetInteger();
        Check(id == static_cast<std::int32_t>(i) &&
                  rows[i].GetValue(1).GetInteger() == id + 1000,
              "Stress table contents are wrong");
    }
    Check(!catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(29)) &&
              catalog.GetIndex("group_idx").GetTree().GetValues(IndexKey(1)).size() == 10 &&
              sql.ExecuteSQL("SELECT id FROM t WHERE group_id = 0 AND name = 'n0'").rows.size() == 1,
          "Stress indexes are inconsistent");
    for (const auto id : catalog.ListIndexes()) { catalog.GetIndex(id).GetTree().Validate(); }
}

void ExerciseDeadlock(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE lock_a (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("CREATE TABLE lock_b (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("INSERT INTO lock_a VALUES (1, 1)");
    setup.ExecuteSQL("INSERT INTO lock_b VALUES (1, 1)");
    SqlEngine first(catalog);
    SqlEngine second(catalog);
    const auto first_id = *first.ExecuteSQL("BEGIN").transaction_id;
    const auto second_id = *second.ExecuteSQL("BEGIN").transaction_id;
    first.ExecuteSQL("UPDATE lock_a SET value = 2 WHERE id = 1");
    second.ExecuteSQL("UPDATE lock_b SET value = 3 WHERE id = 1");
    std::atomic<bool> survivor_done = false;
    std::thread waiter([&] {
        first.ExecuteSQL("UPDATE lock_b SET value = 2 WHERE id = 1");
        survivor_done = true;
    });
    WaitUntil([&] {
        const auto graph = catalog.GetLockManager().GetWaitsForGraph();
        const auto found = graph.find(first_id);
        return found != graph.end() && found->second.count(second_id) != 0;
    }, "Stress deadlock did not establish its first wait edge");
    bool victim = false;
    try { second.ExecuteSQL("UPDATE lock_a SET value = 3 WHERE id = 1"); }
    catch (const DeadlockError&) { victim = true; }
    waiter.join();
    Check(victim && survivor_done && !second.HasActiveTransaction(),
          "Stress deadlock did not abort the victim");
    first.ExecuteSQL("COMMIT");
}

void RunRound(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 4);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(30), value INTEGER)");
        for (int id = 0; id < 30; ++id) {
            sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                           std::to_string(id % 3) + ", 'n" + std::to_string(id) +
                           "', " + std::to_string(id) + ")");
        }
        sql.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
        sql.ExecuteSQL("CREATE INDEX name_idx ON t(name)");
        sql.ExecuteSQL("CREATE INDEX group_name_idx ON t(group_id, name)");
        BPlusTreeOptions options;
        options.unique = false;
        catalog.CreateIndex("group_idx", catalog.GetTable("t").GetTableId(), 1, options);
        database->Close();
    }
    {
        auto database = Database::Open(path, 32);
        auto& catalog = database->GetCatalog();
        SqlEngine historical(catalog, IsolationLevel::SnapshotIsolation);
        historical.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");

        std::atomic<bool> failed = false;
        std::mutex failure_mutex;
        std::string failure_message;
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 3; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    SqlEngine sql(catalog);
                    for (int id = worker * 10; id < (worker + 1) * 10; ++id) {
                        sql.ExecuteSQL("UPDATE t SET value = " + std::to_string(id + 1000) +
                                       " WHERE id = " + std::to_string(id));
                    }
                } catch (const std::exception& error) {
                    const std::lock_guard<std::mutex> lock(failure_mutex);
                    failed = true;
                    failure_message = error.what();
                }
            });
        }
        for (int reader = 0; reader < 3; ++reader) {
            workers.emplace_back([&, reader] {
                try {
                    SqlEngine sql(catalog, IsolationLevel::ReadCommitted);
                    for (int pass = 0; pass < 20; ++pass) {
                        static_cast<void>(sql.ExecuteSQL(
                            "SELECT id FROM t WHERE id >= " + std::to_string(reader * 5) +
                            " AND id <= " + std::to_string(reader * 5 + 10)));
                        static_cast<void>(sql.ExecuteSQL(
                            "SELECT id FROM t WHERE group_id = " + std::to_string(reader)));
                    }
                } catch (const std::exception& error) {
                    const std::lock_guard<std::mutex> lock(failure_mutex);
                    failed = true;
                    failure_message = error.what();
                }
            });
        }
        for (auto& worker : workers) { worker.join(); }
        if (failed) { throw std::runtime_error("Concurrent MVCC stress worker failed: " + failure_message); }
        const auto old_rows = historical.ExecuteSQL("SELECT id, value FROM t ORDER BY id").rows;
        Check(old_rows.size() == 30 && old_rows[10].GetValue(1) == Value::Integer(10),
              "Long snapshot changed during concurrent writes");
        historical.ExecuteSQL("COMMIT");

        SqlEngine sql(catalog);
        sql.ExecuteSQL("DELETE FROM t WHERE id = 29");
        Check(catalog.Vacuum() == 1, "Stress vacuum did not reclaim tombstone");
        SqlEngine aborted(catalog, IsolationLevel::SnapshotIsolation);
        aborted.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        aborted.ExecuteSQL("UPDATE t SET value = 9999 WHERE id = 0");
        aborted.ExecuteSQL("ROLLBACK");
        ExerciseDeadlock(catalog);
        Verify(catalog);
        database->Checkpoint();

        SqlEngine loser(catalog, IsolationLevel::SnapshotIsolation);
        loser.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        loser.ExecuteSQL("UPDATE t SET value = 7777 WHERE id = 0");
        database->Flush();
        // Crash simulation: active loser is deliberately neither aborted nor closed.
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        auto database = Database::Open(path, reopen == 0 ? 2 : 1);
        Verify(database->GetCatalog());
        Check(database->GetCatalog().Vacuum() == 0,
              "Post-recovery vacuum found inconsistent MVCC garbage");
        if (reopen == 0) { database->Checkpoint(); }
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-stress-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        for (int round = 0; round < 2; ++round) {
            RunRound(directory / ("round-" + std::to_string(round) + ".udb"));
        }
        std::cout << "MVCC concurrent stress tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
