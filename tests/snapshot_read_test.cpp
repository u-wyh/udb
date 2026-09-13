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

Tuple Row(const Schema& schema, std::int32_t id, const std::string& name) {
    return Tuple(schema, {Value::Integer(id), Value::Varchar(name)});
}

void TestSnapshotScans(const std::filesystem::path& path) {
    auto database = Database::Create(path, 3);
    auto& catalog = database->GetCatalog();
    const Schema schema({Column("id", TypeId::INTEGER),
                         Column("name", TypeId::VARCHAR, 40)});
    const auto table_id = catalog.CreateTable("t", schema).GetTableId();
    auto& heap = catalog.GetTableHeap(table_id);
    const auto changed = heap.InsertRecord(Row(schema, 1, "old").Serialize(schema));
    const auto stable = heap.InsertRecord(Row(schema, 2, "stable").Serialize(schema));
    const auto future = heap.InsertRecord(Row(schema, 3, "future").Serialize(schema));
    const auto deleted_later = heap.InsertRecord(Row(schema, 4, "before-delete").Serialize(schema));
    const auto already_deleted = heap.InsertRecord(Row(schema, 5, "gone").Serialize(schema));
    catalog.CreateIndex("id_idx", table_id, 0);

    TransactionManager manager(&catalog.GetBufferPoolManager(), catalog.GetLogManager(),
                               &catalog.GetLockManager());
    auto& reader = manager.Begin(IsolationLevel::SnapshotIsolation);
    const auto snapshot = reader.GetReadTimestamp();
    manager.AppendUndoRecord(reader, changed, heap.GetRecord(changed), {snapshot, false});
    Check(heap.UpdateRecord(changed, Row(schema, 1, "new").Serialize(schema)),
          "Cannot prepare updated current tuple");
    heap.SetTupleMeta(changed, {snapshot + 1, false});
    heap.SetTupleMeta(stable, {snapshot, false});
    heap.SetTupleMeta(future, {snapshot + 1, false});
    manager.AppendUndoRecord(reader, deleted_later, heap.GetRecord(deleted_later),
                             {snapshot, false});
    heap.SetTupleMeta(deleted_later, {snapshot + 1, true});
    heap.SetTupleMeta(already_deleted, {snapshot, true});

    SqlEngine sql(catalog);
    ExecutionContext context(reader, catalog.GetLockManager(), manager);
    auto result = sql.ExecuteSQL("SELECT name FROM t WHERE name = 'old'", context);
    Check(result.type == PlanType::SeqScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Varchar("old"),
          "SeqScan did not reconstruct the snapshot row");

    result = sql.ExecuteSQL("SELECT name FROM t WHERE id = 1", context);
    Check(result.type == PlanType::IndexScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Varchar("old"),
          "IndexScan did not reconstruct the snapshot row");
    result = sql.ExecuteSQL("SELECT id FROM t WHERE id = 1", context);
    Check(result.type == PlanType::IndexOnlyScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Integer(1),
          "Snapshot IndexOnlyScan did not safely fall back to tuple visibility");

    result = sql.ExecuteSQL(
        "SELECT id, name FROM t WHERE id >= 1 AND id <= 5 ORDER BY id", context);
    Check(result.type == PlanType::IndexRangeScan && result.rows.size() == 3 &&
              result.rows[0].GetValue(0) == Value::Integer(1) &&
              result.rows[1].GetValue(0) == Value::Integer(2) &&
              result.rows[2].GetValue(0) == Value::Integer(4) &&
              result.rows[2].GetValue(1) == Value::Varchar("before-delete"),
          "Range scan snapshot visibility is wrong");
    result = sql.ExecuteSQL("SELECT COUNT(*) FROM t", context);
    Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::BigInt(3),
          "Aggregate TableScan ignored snapshot visibility");
    Check(reader.GetSharedRowLocks().empty(),
          "Snapshot reads acquired ordinary row shared locks");
    manager.Commit(reader);

    SqlEngine command(catalog);
    const auto begun = command.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    Check(begun.type == PlanType::Begin && command.HasActiveTransaction(),
          "SQL BEGIN did not accept Snapshot Isolation");
    command.ExecuteSQL("ROLLBACK");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-snapshot-read-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSnapshotScans(directory / "snapshot.udb");
        std::cout << "Snapshot read tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
