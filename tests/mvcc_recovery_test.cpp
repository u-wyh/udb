#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct CrashState {
    RID updated;
    RID deleted;
    RID loser;
    timestamp_t updated_timestamp;
    timestamp_t deleted_timestamp;
};

void SaveState(const std::filesystem::path& path, const CrashState& state) {
    std::ofstream output(path);
    output << state.updated.page_id << ' ' << state.updated.slot_id << ' '
           << state.deleted.page_id << ' ' << state.deleted.slot_id << ' '
           << state.loser.page_id << ' ' << state.loser.slot_id << ' '
           << state.updated_timestamp << ' ' << state.deleted_timestamp << '\n';
    output.flush();
    Check(static_cast<bool>(output), "Cannot save crash test state");
}

CrashState LoadState(const std::filesystem::path& path) {
    CrashState state;
    std::uint64_t updated_slot = 0;
    std::uint64_t deleted_slot = 0;
    std::uint64_t loser_slot = 0;
    std::ifstream input(path);
    input >> state.updated.page_id >> updated_slot
          >> state.deleted.page_id >> deleted_slot
          >> state.loser.page_id >> loser_slot
          >> state.updated_timestamp >> state.deleted_timestamp;
    Check(static_cast<bool>(input), "Cannot load crash test state");
    state.updated.slot_id = static_cast<slot_id_t>(updated_slot);
    state.deleted.slot_id = static_cast<slot_id_t>(deleted_slot);
    state.loser.slot_id = static_cast<slot_id_t>(loser_slot);
    return state;
}

[[noreturn]] void BuildCrashImage(const std::filesystem::path& database_path,
                                  const std::filesystem::path& state_path) {
    try {
        {
            auto database = Database::Create(database_path, 2);
            SqlEngine sql(database->GetCatalog());
            sql.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(40))");
            sql.ExecuteSQL("INSERT INTO t VALUES (1, 5, 'one')");
            sql.ExecuteSQL("INSERT INTO t VALUES (2, 5, 'two')");
            sql.ExecuteSQL("INSERT INTO t VALUES (3, 7, 'three')");
            sql.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
            sql.ExecuteSQL("CREATE INDEX name_idx ON t(name)");
            sql.ExecuteSQL("CREATE INDEX group_name_idx ON t(group_id, name)");
            BPlusTreeOptions options;
            options.unique = false;
            const auto table_id = database->GetCatalog().GetTable("t").GetTableId();
            database->GetCatalog().CreateIndex("group_idx", table_id, 1, options);
            database->Close();
        }

        auto database = Database::Open(database_path, 2);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        const auto deleted = *catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(2));
        const auto loser = *catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(3));
        sql.ExecuteSQL("UPDATE t SET id = 10, group_id = 6, name = 'new' WHERE id = 1");
        sql.ExecuteSQL("UPDATE t SET name = 'newer' WHERE id = 10");
        const auto updated = *catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(10));
        const auto updated_timestamp = catalog.GetTableHeap("t").GetTupleMeta(updated).timestamp;
        sql.ExecuteSQL("DELETE FROM t WHERE id = 2");
        const auto deleted_timestamp = catalog.GetTableHeap("t").GetTupleMeta(deleted).timestamp;

        SqlEngine active(catalog, IsolationLevel::SnapshotIsolation);
        active.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        active.ExecuteSQL("UPDATE t SET id = 99, name = 'loser' WHERE id = 10");
        active.ExecuteSQL("DELETE FROM t WHERE id = 3");
        database->Flush();
        SaveState(state_path, {updated, deleted, loser,
                               updated_timestamp, deleted_timestamp});
        ::_exit(0);
    } catch (...) {
        ::_exit(2);
    }
}

void CheckRecovered(Database& database, const CrashState& state) {
    auto& catalog = database.GetCatalog();
    SqlEngine sql(catalog, IsolationLevel::SnapshotIsolation);
    const auto rows = sql.ExecuteSQL("SELECT id, group_id, name FROM t ORDER BY id").rows;
    Check(rows.size() == 2 && rows[0].GetValue(0) == Value::Integer(3) &&
              rows[1].GetValue(0) == Value::Integer(10) &&
              rows[1].GetValue(2) == Value::Varchar("newer"),
          "Recovered visible tuples are incorrect");
    Check(sql.ExecuteSQL("SELECT * FROM t WHERE id = 2").rows.empty() &&
              sql.ExecuteSQL("SELECT * FROM t WHERE id = 99").rows.empty(),
          "Recovery retained a deleted or loser tuple");

    auto& heap = catalog.GetTableHeap("t");
    const auto updated_meta = heap.GetTupleMeta(state.updated);
    const auto deleted_meta = heap.GetTupleMeta(state.deleted);
    const auto loser_meta = heap.GetTupleMeta(state.loser);
    Check(updated_meta.timestamp == state.updated_timestamp && !updated_meta.is_deleted &&
              !TransactionManager::IsTransactionTimestamp(updated_meta.timestamp),
          "Committed UPDATE TupleMeta was not recovered");
    Check(deleted_meta.timestamp == state.deleted_timestamp && deleted_meta.is_deleted &&
              !TransactionManager::IsTransactionTimestamp(deleted_meta.timestamp),
          "Committed logical DELETE TupleMeta was not recovered");
    Check(!loser_meta.is_deleted && !TransactionManager::IsTransactionTimestamp(loser_meta.timestamp),
          "Loser MVCC TupleMeta was not undone");

    Check(catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(10)) == state.updated &&
              !catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(1)) &&
              !catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(2)) &&
              !catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(99)) &&
              catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(3)) == state.loser,
          "Recovered unique index does not match visible tuples");
    Check(catalog.GetIndex("group_idx").GetTree().GetValues(IndexKey(6)) ==
              std::vector<RID>{state.updated},
          "Recovered non-unique index is incorrect");
    Check(sql.ExecuteSQL("SELECT id FROM t WHERE group_id = 6 AND name = 'newer'").rows.size() == 1,
          "Recovered composite index is incorrect");
    for (const auto id : catalog.ListIndexes()) { catalog.GetIndex(id).GetTree().Validate(); }

    const auto recovered_clock = TransactionManager::GetLastCommitTimestamp();
    Check(recovered_clock >= state.deleted_timestamp,
          "Recovery did not restore the global commit timestamp");
    auto& manager = catalog.GetTransactionManager();
    auto& transaction = manager.Begin(IsolationLevel::SnapshotIsolation);
    Check(transaction.GetReadTimestamp() >= recovered_clock,
          "Post-recovery snapshot timestamp moved backwards");
    manager.Commit(transaction);
    Check(*transaction.GetCommitTimestamp() > recovered_clock,
          "Post-recovery commit timestamp did not advance");
}

void TestMvccRecovery(const std::filesystem::path& directory) {
    const auto database_path = directory / "database.udb";
    const auto state_path = directory / "state.txt";
    const auto child = ::fork();
    Check(child >= 0, "Cannot fork crash writer");
    if (child == 0) { BuildCrashImage(database_path, state_path); }
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Crash writer failed");
    const auto state = LoadState(state_path);

    auto wal_path = database_path;
    wal_path.replace_extension(".wal");
    {
        std::ofstream output(wal_path, std::ios::binary | std::ios::app);
        output.write("tail", 4);
    }
    {
        auto database = Database::Open(database_path, 2);
        CheckRecovered(*database, state);
        database->Flush();
    }
    {
        auto database = Database::Open(database_path, 1);
        CheckRecovered(*database, state);
        Check(database->GetCatalog().Vacuum() == 1,
              "Vacuum failed after MVCC recovery");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-recovery-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestMvccRecovery(directory);
        std::cout << "MVCC WAL recovery tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
