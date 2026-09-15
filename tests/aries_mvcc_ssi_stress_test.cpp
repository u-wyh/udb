#include "udb/database.h"
#include "udb/recovery_manager.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void ValidateIndexes(Catalog& catalog) {
    for (const auto index_id : catalog.ListIndexes()) {
        catalog.GetIndex(index_id).GetTree().Validate();
    }
}

void VerifyRecovered(Catalog& catalog) {
    SqlEngine sql(catalog, IsolationLevel::SnapshotIsolation);
    const auto rows = sql.ExecuteSQL("SELECT id, value FROM t ORDER BY id").rows;
    Check(rows.size() == 110, "Recovered stress row count is wrong");
    for (const auto& row : rows) {
        const auto id = row.GetValue(0).GetInteger();
        Check((id < 80 || id >= 130) && id != 1005 && id != 5000,
              "Loser or deleted key survived recovery");
        std::int32_t expected = id;
        if (id < 16) { expected = id + 1000; }
        if (id == 0) { expected = 9000; }
        Check(row.GetValue(1).GetInteger() == expected,
              "Committed stress update was lost");
    }
    Check(sql.ExecuteSQL("SELECT id FROM t WHERE id = 5").rows.size() == 1 &&
          sql.ExecuteSQL("SELECT id FROM t WHERE id = 6").rows.size() == 1 &&
          sql.ExecuteSQL("SELECT id FROM t WHERE id = 1005").rows.empty() &&
          sql.ExecuteSQL("SELECT id FROM t WHERE id = 5000").rows.empty(),
          "Index visibility disagrees with recovered tuples");
    const auto aux = sql.ExecuteSQL("SELECT id FROM aux ORDER BY id").rows;
    Check(!aux.empty() && aux.front().GetValue(0) == Value::Integer(42) && aux.size() <= 2,
          "Winner committed after fuzzy checkpoint was lost");
    ValidateIndexes(catalog);
}

void RunConcurrentUpdates(Catalog& catalog) {
    std::atomic<bool> failed = false;
    std::mutex failure_mutex;
    std::string failure;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                SqlEngine sql(catalog, IsolationLevel::RepeatableRead);
                sql.ExecuteSQL("BEGIN");
                for (int id = worker * 4; id < worker * 4 + 4; ++id) {
                    sql.ExecuteSQL("UPDATE t SET value = " + std::to_string(id + 1000) +
                                   " WHERE id = " + std::to_string(id));
                }
                sql.ExecuteSQL("COMMIT");
            } catch (const std::exception& error) {
                const std::lock_guard<std::mutex> lock(failure_mutex);
                failed = true;
                failure = error.what();
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    if (failed) { throw std::runtime_error("Concurrent recovery worker failed: " + failure); }
}

void ExerciseSsi(Catalog& catalog) {
    SqlEngine first(catalog);
    SqlEngine second(catalog);
    first.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
    second.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
    first.ExecuteSQL("SELECT COUNT(*) FROM t WHERE value >= 0");
    second.ExecuteSQL("SELECT COUNT(*) FROM t WHERE value >= 0");
    first.ExecuteSQL("UPDATE t SET value = 9000 WHERE id = 0");
    first.ExecuteSQL("COMMIT");
    second.ExecuteSQL("UPDATE t SET value = 9001 WHERE id = 1");
    bool rejected = false;
    try { second.ExecuteSQL("COMMIT"); }
    catch (const SerializationFailure&) { rejected = true; }
    Check(rejected, "SSI stress allowed a dangerous structure to commit");
}

void CreateFixture(const std::filesystem::path& path) {
    auto database = Database::Create(path, 32);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(40), value INTEGER)");
    sql.ExecuteSQL("CREATE TABLE aux (id INTEGER)");
    sql.ExecuteSQL("BEGIN");
    for (int id = 0; id < 160; ++id) {
        sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                       std::to_string(id % 5) + ", 'name" + std::to_string(id) +
                       "', " + std::to_string(id) + ")");
    }
    sql.ExecuteSQL("COMMIT");
    sql.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
    sql.ExecuteSQL("CREATE INDEX name_idx ON t(name)");
    BPlusTreeOptions options;
    options.unique = false;
    catalog.CreateIndex("group_idx", catalog.GetTable("t").GetTableId(), 1, options);
    ValidateIndexes(catalog);
    database->Close();
}

void ProduceCrash(const std::filesystem::path& path) {
    auto database = Database::Open(path, 6);
    auto& catalog = database->GetCatalog();
    SqlEngine long_reader(catalog);
    long_reader.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    Check(long_reader.ExecuteSQL("SELECT value FROM t WHERE id = 0").rows[0].GetValue(0) ==
              Value::Integer(0), "Long snapshot fixture is wrong");
    RunConcurrentUpdates(catalog);
    Check(long_reader.ExecuteSQL("SELECT value FROM t WHERE id = 0").rows[0].GetValue(0) ==
              Value::Integer(0), "Concurrent writers changed a long snapshot");
    long_reader.ExecuteSQL("COMMIT");
    ExerciseSsi(catalog);

    SqlEngine maintenance(catalog);
    maintenance.ExecuteSQL("DELETE FROM t WHERE id >= 80 AND id < 130");
    static_cast<void>(catalog.Vacuum());
    ValidateIndexes(catalog);
    database->Flush();

    SqlEngine loser(catalog, IsolationLevel::SnapshotIsolation);
    SqlEngine winner(catalog);
    loser.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    loser.ExecuteSQL("UPDATE t SET id = 1005 WHERE id = 5");
    loser.ExecuteSQL("DELETE FROM t WHERE id = 6");
    loser.ExecuteSQL("INSERT INTO t VALUES (5000, 0, 'loser', 5000)");
    winner.ExecuteSQL("BEGIN");
    winner.ExecuteSQL("INSERT INTO aux VALUES (42)");
    const auto wal_size = std::filesystem::file_size(database->GetLogManager().GetPath());
    database->Checkpoint();
    Check(database->GetLogManager().HasActiveTransactions() &&
          std::filesystem::file_size(database->GetLogManager().GetPath()) < wal_size,
          "Stress fuzzy checkpoint did not retain and truncate active WAL");
    winner.ExecuteSQL("COMMIT");
    // Simulated process crash leaves the other transaction active.
}

void InterruptRecovery(const std::filesystem::path& path) {
    auto wal_path = path;
    wal_path.replace_extension(".wal");
    {
        DiskManager disk(path);
        LogManager log(wal_path);
        const auto analysis = RecoveryManager::Analyze(log);
        Check(!analysis.losers.empty(), "Stress crash has no loser transaction");
        RecoveryManager::Redo(disk, log, analysis);
        // Crash during redo, before undo begins.
    }
    {
        DiskManager disk(path);
        LogManager log(wal_path);
        const auto analysis = RecoveryManager::Analyze(log);
        RecoveryManager::Redo(disk, log, analysis);
        const auto partial = RecoveryManager::Undo(disk, log, analysis, 2);
        Check(!partial.complete && partial.compensation_records == 2,
              "Stress recovery did not stop during Undo");
        // A second crash leaves durable CLRs and their page images.
    }
    {
        std::ofstream tail(wal_path, std::ios::binary | std::ios::app);
        tail.write("truncated", 9);
    }
}

void FinishAndVerify(const std::filesystem::path& path) {
    timestamp_t timestamp = 0;
    for (int reopen = 0; reopen < 3; ++reopen) {
        auto database = Database::Open(path, reopen == 0 ? 2 : 1);
        auto& catalog = database->GetCatalog();
        VerifyRecovered(catalog);
        Check(TransactionManager::GetLastCommitTimestamp() >= timestamp,
              "Commit timestamp moved backward after reopen");
        timestamp = TransactionManager::GetLastCommitTimestamp();
        if (reopen == 0) {
            Check(catalog.Vacuum() <= 3, "Recovery left excessive MVCC garbage");
            SqlEngine sql(catalog);
            sql.ExecuteSQL("INSERT INTO aux VALUES (43)");
            Check(TransactionManager::GetLastCommitTimestamp() > timestamp,
                  "Commit timestamp did not advance after recovery");
            timestamp = TransactionManager::GetLastCommitTimestamp();
            database->Checkpoint();
            Check(database->GetLogManager().GetRecords().empty(),
                  "Final checkpoint did not reclaim completed WAL");
        }
        database->Close();
    }
}

void RunRound(const std::filesystem::path& path) {
    CreateFixture(path);
    ProduceCrash(path);
    InterruptRecovery(path);
    FinishAndVerify(path);
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-aries-mvcc-ssi-stress-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        for (int round = 0; round < 2; ++round) {
            RunRound(directory / ("round-" + std::to_string(round) + ".udb"));
        }
        std::cout << "ARIES MVCC SSI stress tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
