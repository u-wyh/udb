#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/transaction.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected error");
}

std::size_t CountType(const LogManager& log, LogRecordType type) {
    std::size_t count = 0;
    for (const auto& record : log.GetRecords()) {
        if (record.GetType() == type) { ++count; }
    }
    return count;
}

void TestGuardLoggingAndFlushOrder(const std::filesystem::path& directory) {
    LogManager log(directory / "guards.wal");
    DiskManager disk(directory / "guards.udb");
    BufferPoolManager pool(disk, 1, &log);
    TransactionManager transactions(&pool, &log);
    auto& transaction = transactions.Begin();
    pool.SetActiveTransaction(&transaction);

    page_id_t first_id = -1;
    {
        auto guard = pool.NewPageGuard();
        first_id = guard.GetPageId();
        guard.GetPage().data.fill('a');
    }
    Check(CountType(log, LogRecordType::PageWrite) == 1,
          "Write guard did not append PAGE_WRITE");
    const auto& write = log.GetRecords().back();
    Check(write.GetType() == LogRecordType::PageWrite && write.GetPageId() == first_id &&
          write.GetBeforeImage() && write.GetAfterImage() &&
          write.GetBeforeImage()->data[0] == 0 && write.GetAfterImage()->data[0] == 'a',
          "Write guard recorded incorrect physical images");
    const auto write_lsn = write.GetLsn();
    Check(log.GetPersistentLsn() && *log.GetPersistentLsn() < write_lsn,
          "PAGE_WRITE unexpectedly reported durable before a flush boundary");

    {
        auto guard = pool.NewPageGuard();
        static_cast<void>(guard);
    }
    Check(log.GetPersistentLsn() && *log.GetPersistentLsn() >= write_lsn &&
          disk.ReadPage(first_id).data[0] == 'a',
          "Victim write reached disk before its WAL became durable");

    const auto writes_before = CountType(log, LogRecordType::PageWrite);
    const auto second_id = disk.GetNextPageId() - 1;
    {
        auto guard = pool.WritePage(second_id);
        static_cast<void>(guard);
    }
    Check(CountType(log, LogRecordType::PageWrite) == writes_before,
          "Unchanged write guard emitted a PAGE_WRITE");

    pool.SetActiveTransaction(nullptr);
    transactions.Commit(transaction);
    Check(log.GetRecords().back().GetType() == LogRecordType::Commit &&
          log.GetPersistentLsn() == log.GetRecords().back().GetLsn(),
          "COMMIT was acknowledged before becoming durable");
}

void TestFlushAndFreeLogging(const std::filesystem::path& directory) {
    LogManager log(directory / "flush.wal");
    DiskManager disk(directory / "flush.udb");
    BufferPoolManager pool(disk, 2, &log);
    const auto [page_id, page] = pool.NewPage();
    page->data.fill('x');
    pool.UnpinPage(page_id, true);
    pool.FlushAllPages();

    TransactionManager transactions(&pool, &log);
    auto& transaction = transactions.Begin();
    pool.SetActiveTransaction(&transaction);
    {
        auto guard = pool.WritePage(page_id);
        guard.GetPage().data.fill('y');
        Reject([&] { pool.FlushPage(page_id); });
    }
    const auto write_lsn = log.GetRecords().back().GetLsn();
    pool.FlushPage(page_id);
    Check(log.GetPersistentLsn() && *log.GetPersistentLsn() >= write_lsn &&
          disk.ReadPage(page_id).data[0] == 'y',
          "FlushPage violated write-ahead ordering");
    Check(pool.DeletePage(page_id), "Logged page free failed");
    const auto& freed = log.GetRecords().back();
    Check(freed.GetType() == LogRecordType::PageFree && freed.GetPageId() == page_id &&
          freed.GetBeforeImage() && freed.GetBeforeImage()->data[0] == 'y',
          "PAGE_FREE did not preserve the immediate page image");
    pool.SetActiveTransaction(nullptr);
    transactions.Commit(transaction);
}

void TestSqlWal(const std::filesystem::path& path) {
    std::size_t record_count = 0;
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, name VARCHAR(100))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'one')");
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'two')");
        sql.ExecuteSQL("ROLLBACK");
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("INSERT INTO t VALUES (3, 'three')");
        sql.ExecuteSQL("COMMIT");

        const auto& log = database->GetLogManager();
        Check(CountType(log, LogRecordType::Begin) == 4 &&
              CountType(log, LogRecordType::Commit) == 3 &&
              CountType(log, LogRecordType::Abort) == 1 &&
              CountType(log, LogRecordType::PageWrite) > 0,
              "SQL transaction lifecycle was not logged");
        Check(log.GetPersistentLsn() == log.GetRecords().back().GetLsn(),
              "Final SQL transaction record is not durable");
        Check(sql.ExecuteSQL("SELECT id FROM t ORDER BY id").rows.size() == 2,
              "WAL integration changed rollback semantics");
        record_count = log.GetRecords().size();
        Check(record_count > 0, "Integrated WAL unexpectedly stayed empty");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        Check(database->GetLogManager().GetRecords().empty(),
              "Clean close did not recycle integrated WAL records");
        SqlEngine sql(database->GetCatalog());
        const auto rows = sql.ExecuteSQL("SELECT id FROM t ORDER BY id").rows;
        Check(rows.size() == 2 && rows[0].GetValue(0) == Value::Integer(1) &&
              rows[1].GetValue(0) == Value::Integer(3),
              "Committed SQL state changed after WAL reopen");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-write-ahead-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestGuardLoggingAndFlushOrder(directory);
        TestFlushAndFreeLogging(directory);
        TestSqlWal(directory / "database.udb");
        std::cout << "Write-ahead rule tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
