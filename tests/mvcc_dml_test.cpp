#include "udb/database.h"
#include "udb/execution_context.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void ReloadIndexes(Catalog& catalog) {
    for (const auto index_id : catalog.ListIndexes()) {
        catalog.GetIndex(index_id).GetTree().ReloadRootFromHeader();
    }
}

void TestMvccDml(const std::filesystem::path& path) {
    RID stable_rid;
    timestamp_t delete_timestamp = 0;
    {
        auto database = Database::Create(path, 2);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, name VARCHAR(40))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'base')");
        sql.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
        stable_rid = *catalog.GetIndex("id_idx").GetTree().GetValue(1);
        auto& heap = catalog.GetTableHeap("t");
        const auto baseline_meta = heap.GetTupleMeta(stable_rid);
        Check(!TransactionManager::IsTransactionTimestamp(baseline_meta.timestamp) &&
                  !baseline_meta.is_deleted,
              "Autocommit INSERT did not receive a committed tuple timestamp");

        TransactionManager manager(&catalog.GetBufferPoolManager(), catalog.GetLogManager(),
                                   &catalog.GetLockManager());
        auto& old_reader = manager.Begin(IsolationLevel::SnapshotIsolation);
        auto& writer = manager.Begin(IsolationLevel::SnapshotIsolation);
        ExecutionContext writer_context(writer, catalog.GetLockManager(), manager);
        Check(sql.ExecuteSQL("UPDATE t SET name = 'committed' WHERE id = 1",
                             writer_context).affected_rows == 1,
              "MVCC UPDATE did not affect the row");
        auto meta = heap.GetTupleMeta(stable_rid);
        Check(TransactionManager::IsTransactionTimestamp(meta.timestamp) &&
                  TransactionManager::DecodeTransactionTimestamp(meta.timestamp) == writer.GetId(),
              "UPDATE current version is not owned by its transaction");
        auto own = sql.ExecuteSQL("SELECT name FROM t WHERE id = 1", writer_context);
        Check(own.rows.size() == 1 && own.rows[0].GetValue(0) == Value::Varchar("committed"),
              "UPDATE is not visible to its writer");
        manager.Commit(writer);
        meta = heap.GetTupleMeta(stable_rid);
        Check(writer.GetCommitTimestamp() == meta.timestamp &&
                  !TransactionManager::IsTransactionTimestamp(meta.timestamp),
              "COMMIT did not stamp the updated tuple");

        ExecutionContext reader_context(old_reader, catalog.GetLockManager(), manager);
        const auto old = sql.ExecuteSQL("SELECT name FROM t WHERE id = 1", reader_context);
        Check(old.rows.size() == 1 && old.rows[0].GetValue(0) == Value::Varchar("base"),
              "Old snapshot did not reconstruct the pre-update row");
        manager.Commit(old_reader);

        auto& rollback_update = manager.Begin(IsolationLevel::SnapshotIsolation);
        ExecutionContext rollback_update_context(rollback_update, catalog.GetLockManager(), manager);
        sql.ExecuteSQL("UPDATE t SET id = 2, name = 'rollback' WHERE id = 1",
                       rollback_update_context);
        Check(sql.ExecuteSQL("SELECT name FROM t WHERE id = 2", rollback_update_context)
                      .rows.at(0).GetValue(0) == Value::Varchar("rollback"),
              "Indexed UPDATE is not visible to its writer");
        manager.Abort(rollback_update, [&] { ReloadIndexes(catalog); });
        Check(catalog.GetIndex("id_idx").GetTree().GetValue(1) == stable_rid &&
                  !catalog.GetIndex("id_idx").GetTree().GetValue(2) &&
                  sql.ExecuteSQL("SELECT name FROM t WHERE id = 1").rows.at(0).GetValue(0) ==
                      Value::Varchar("committed"),
              "UPDATE abort did not restore table and index state");

        auto& rollback_insert = manager.Begin(IsolationLevel::SnapshotIsolation);
        ExecutionContext rollback_insert_context(rollback_insert, catalog.GetLockManager(), manager);
        const auto inserted = sql.ExecuteSQL("INSERT INTO t VALUES (3, 'temporary')",
                                             rollback_insert_context);
        Check(inserted.inserted_rid &&
                  sql.ExecuteSQL("SELECT name FROM t WHERE id = 3", rollback_insert_context)
                          .rows.size() == 1,
              "INSERT is not visible to its writer");
        manager.Abort(rollback_insert, [&] { ReloadIndexes(catalog); });
        Check(sql.ExecuteSQL("SELECT * FROM t WHERE id = 3").rows.empty() &&
                  !catalog.GetIndex("id_idx").GetTree().GetValue(3),
              "INSERT abort left a table row or index entry");

        auto& rollback_delete = manager.Begin(IsolationLevel::SnapshotIsolation);
        ExecutionContext rollback_delete_context(rollback_delete, catalog.GetLockManager(), manager);
        Check(sql.ExecuteSQL("DELETE FROM t WHERE id = 1", rollback_delete_context)
                      .affected_rows == 1 &&
                  sql.ExecuteSQL("SELECT * FROM t WHERE id = 1", rollback_delete_context)
                      .rows.empty() && heap.GetTupleMeta(stable_rid).is_deleted,
              "DELETE is not a writer-visible logical tombstone");
        manager.Abort(rollback_delete, [&] { ReloadIndexes(catalog); });
        Check(!heap.GetTupleMeta(stable_rid).is_deleted &&
                  sql.ExecuteSQL("SELECT * FROM t WHERE id = 1").rows.size() == 1,
              "DELETE abort did not restore the live tuple");

        auto& commit_delete = manager.Begin(IsolationLevel::SnapshotIsolation);
        ExecutionContext commit_delete_context(commit_delete, catalog.GetLockManager(), manager);
        sql.ExecuteSQL("DELETE FROM t WHERE id = 1", commit_delete_context);
        manager.Commit(commit_delete);
        const auto tombstone = heap.GetTupleMeta(stable_rid);
        Check(tombstone.is_deleted && commit_delete.GetCommitTimestamp() == tombstone.timestamp &&
                  heap.GetRecord(stable_rid).Size() != 0 &&
                  sql.ExecuteSQL("SELECT * FROM t WHERE id = 1").rows.empty(),
              "Committed DELETE did not retain an invisible physical tombstone");
        delete_timestamp = tombstone.timestamp;
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        const auto meta = catalog.GetTableHeap("t").GetTupleMeta(stable_rid);
        Check(meta == TupleMeta{delete_timestamp, true} &&
                  catalog.GetTableHeap("t").GetRecord(stable_rid).Size() != 0 &&
                  sql.ExecuteSQL("SELECT * FROM t").rows.empty() &&
                  !catalog.GetIndex("id_idx").GetTree().GetValue(1),
              "Reopen lost committed logical-delete or index state");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-dml-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestMvccDml(directory / "mvcc.udb");
        std::cout << "MVCC DML tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
