#include "udb/database.h"
#include "udb/recovery_manager.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <iostream>
#include <set>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

Page Filled(char value) {
    Page page;
    page.data.fill(value);
    return page;
}

void TestInterleavedBoundary(const std::filesystem::path& directory) {
    const auto path = directory / "interleaved.wal";
    lsn_t retained_begin = 0;
    std::uintmax_t original_size = 0;
    {
        LogManager log(path);
        log.Append(LogRecord::Begin(1));
        log.Append(LogRecord::PageWrite(1, 0, Page{}, Filled('a')));
        log.Append(LogRecord::Commit(1, 1));
        retained_begin = log.Append(LogRecord::Begin(2));
        const auto active_last = log.Append(
            LogRecord::PageWrite(2, 1, Page{}, Filled('b')));
        log.Append(LogRecord::Begin(3));
        const auto dirty_lsn = log.Append(
            LogRecord::PageWrite(3, 2, Page{}, Filled('c')));
        log.Append(LogRecord::Commit(3, 2));
        log.Flush();
        original_size = std::filesystem::file_size(path);
        const LogCheckpoint checkpoint{
            log.GetNextLsn(), {{2, active_last}}, {{2, dirty_lsn}}};
        Check(log.TruncateForCheckpoint(checkpoint) == 3,
              "WAL truncation removed the wrong prefix");
        Check(log.GetRecords().front().GetLsn() == retained_begin &&
              log.GetRecords().front().GetType() == LogRecordType::Begin &&
              std::filesystem::file_size(path) < original_size,
              "WAL truncation did not retain a complete required transaction");
    }
    {
        LogManager log(path);
        const auto analysis = RecoveryManager::Analyze(log);
        Check(analysis.losers == std::set<transaction_id_t>{2} &&
              analysis.winners == std::set<transaction_id_t>{3},
              "Truncated interleaved WAL could not be analyzed");
    }
}

void TestDatabaseTruncationRecovery(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 2);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, value VARCHAR(100))");
        database->Close();
    }
    lsn_t first_retained = 0;
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'winner')");
        database->Flush();
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'loser')");
        const auto before = std::filesystem::file_size(
            database->GetLogManager().GetPath());
        database->Checkpoint();
        const auto& records = database->GetLogManager().GetRecords();
        Check(!records.empty() && records.front().GetType() == LogRecordType::Begin &&
              records.front().GetLsn() > 0 &&
              std::filesystem::file_size(database->GetLogManager().GetPath()) < before,
              "Fuzzy checkpoint did not reclaim a safe WAL prefix");
        first_retained = records.front().GetLsn();
        const auto checkpoint = database->GetLogManager().ReadCheckpoint();
        Check(checkpoint && checkpoint->transaction_table.size() == 1 &&
              checkpoint->transaction_table.begin()->second >= first_retained,
              "Checkpoint references were not retained in WAL");
        // Simulated crash with the active transaction retained in the WAL suffix.
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        const auto rows = sql.ExecuteSQL("SELECT * FROM t ORDER BY id").rows;
        Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(1),
              "Recovery after WAL prefix truncation lost winner or retained loser");
        Check(database->GetLogManager().GetNextLsn() > first_retained,
              "Post-truncation LSN sequence moved backward");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-wal-truncation-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestInterleavedBoundary(directory);
        TestDatabaseTruncationRecovery(directory / "database.udb");
        std::cout << "WAL truncation tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
