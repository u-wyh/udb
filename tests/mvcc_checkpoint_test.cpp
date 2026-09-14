#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

[[noreturn]] void CreateCheckpoint(const std::filesystem::path& path,
                                   const std::filesystem::path& timestamp_path) {
    try {
        auto database = Database::Create(path, 2);
        auto& catalog = database->GetCatalog();
        SqlEngine writer(catalog);
        writer.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(30))");
        writer.ExecuteSQL("INSERT INTO t VALUES (1, 5, 'one')");
        writer.ExecuteSQL("INSERT INTO t VALUES (2, 5, 'two')");
        writer.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
        writer.ExecuteSQL("CREATE INDEX group_name_idx ON t(group_id, name)");
        BPlusTreeOptions options;
        options.unique = false;
        catalog.CreateIndex("group_idx", catalog.GetTable("t").GetTableId(), 1, options);

        SqlEngine snapshot(catalog, IsolationLevel::SnapshotIsolation);
        snapshot.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
        writer.ExecuteSQL("UPDATE t SET id = 10, group_id = 6, name = 'ten' WHERE id = 1");
        writer.ExecuteSQL("DELETE FROM t WHERE id = 2");
        snapshot.ExecuteSQL("COMMIT");
        Check(catalog.Vacuum() == 1, "Pre-checkpoint vacuum did not reclaim tombstone");
        const auto timestamp = TransactionManager::GetLastCommitTimestamp();
        database->Checkpoint();
        auto wal = path;
        wal.replace_extension(".wal");
        Check(std::filesystem::file_size(wal) == 0 &&
                  database->GetLogManager().GetRecords().empty(),
              "Checkpoint did not reclaim WAL");
        std::ofstream output(timestamp_path);
        output << timestamp << '\n';
        output.flush();
        Check(static_cast<bool>(output), "Cannot save checkpoint timestamp");
        ::_exit(0);
    } catch (...) {
        ::_exit(2);
    }
}

void TestCheckpointReopen(const std::filesystem::path& directory) {
    const auto path = directory / "database.udb";
    const auto timestamp_path = directory / "timestamp.txt";
    const auto child = ::fork();
    Check(child >= 0, "Cannot fork checkpoint writer");
    if (child == 0) { CreateCheckpoint(path, timestamp_path); }
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Checkpoint writer failed");
    timestamp_t saved_timestamp = 0;
    std::ifstream(timestamp_path) >> saved_timestamp;
    Check(saved_timestamp != 0, "Invalid saved checkpoint timestamp");

    auto database = Database::Open(path, 1);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog, IsolationLevel::SnapshotIsolation);
    const auto rows = sql.ExecuteSQL("SELECT name, id FROM t ORDER BY id").rows;
    Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Varchar("ten") &&
              rows[0].GetValue(1) == Value::Integer(10),
          "Checkpoint reopen changed visible MVCC state");
    Check(TransactionManager::GetLastCommitTimestamp() >= saved_timestamp,
          "Metadata did not restore checkpoint commit timestamp");
    Check(catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(10)).has_value() &&
              !catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(1)) &&
              !catalog.GetIndex("id_idx").GetTree().GetValue(IndexKey(2)) &&
              catalog.GetIndex("group_idx").GetTree().GetValues(IndexKey(6)).size() == 1 &&
              sql.ExecuteSQL("SELECT id FROM t WHERE group_id = 6 AND name = 'ten'").rows.size() == 1,
          "Indexes are inconsistent after checkpoint reopen");
    for (const auto id : catalog.ListIndexes()) { catalog.GetIndex(id).GetTree().Validate(); }

    auto& manager = catalog.GetTransactionManager();
    auto& transaction = manager.Begin(IsolationLevel::SnapshotIsolation);
    Check(transaction.GetReadTimestamp() >= saved_timestamp,
          "Reopened snapshot timestamp moved backwards");
    manager.Commit(transaction);
    Check(*transaction.GetCommitTimestamp() > saved_timestamp,
          "Reopened commit timestamp did not advance");
    sql.ExecuteSQL("UPDATE t SET id = 11 WHERE id = 10");
    database->Checkpoint();
    database->Close();

    database = Database::Open(path, 1);
    SqlEngine reopened(database->GetCatalog(), IsolationLevel::SnapshotIsolation);
    Check(reopened.ExecuteSQL("SELECT id FROM t").rows[0].GetValue(0) == Value::Integer(11) &&
              database->GetCatalog().GetIndex("id_idx").GetTree().GetValue(IndexKey(11)).has_value(),
          "Repeated checkpoint/reopen lost data or index state");
    database->Close();
}

void TestVersionFourCompatibility(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        database->Close();
    }
    auto metadata = path;
    metadata.replace_extension(".meta");
    std::ifstream input(metadata, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Check(bytes.size() == 56, "Unexpected version 5 empty metadata size");
    bytes[8] = 4;
    bytes.erase(24, 8);
    std::ofstream output(metadata, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    auto database = Database::Open(path, 1);
    Check(database->GetCatalog().ListTables().empty(), "Version 4 metadata compatibility failed");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-checkpoint-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestCheckpointReopen(directory);
        TestVersionFourCompatibility(directory / "legacy.udb");
        std::cout << "MVCC checkpoint and reopen tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
