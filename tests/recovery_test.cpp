#include "udb/database.h"
#include "udb/recovery_manager.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <fstream>
#include <iostream>

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

void TestPhysicalRecovery(const std::filesystem::path& directory) {
    const auto before = Filled('a');
    const auto after = Filled('b');
    DiskManager disk(directory / "physical.udb");
    const auto page0 = disk.AllocatePage();
    disk.WritePage(page0, before);
    const auto page1 = disk.GetNextPageId();
    const auto page2 = page1 + 1;

    LogManager log(directory / "physical.wal");
    log.Append(LogRecord::Begin(1));
    log.Append(LogRecord::PageWrite(1, page0, before, after));
    log.Append(LogRecord::PageAllocate(1, page1));
    log.Append(LogRecord::PageWrite(1, page1, Page{}, after));
    log.Append(LogRecord::Commit(1));
    log.Append(LogRecord::Begin(2));
    log.Append(LogRecord::PageAllocate(2, page2));
    log.Append(LogRecord::PageWrite(2, page2, Page{}, Filled('x')));
    log.Flush();

    auto states = RecoveryManager::Recover(disk, log);
    disk.ApplyRecoveryPageStates(states);
    Check(disk.ReadPage(page0).data == after.data && disk.ReadPage(page1).data == after.data &&
          !disk.IsPageAllocated(page2), "REDO committed / UNDO loser recovery is wrong");

    LogManager free_log(directory / "free.wal");
    free_log.Append(LogRecord::Begin(3));
    free_log.Append(LogRecord::PageFree(3, page0, after));
    free_log.Flush();
    disk.DeallocatePage(page0);
    states = RecoveryManager::Recover(disk, free_log);
    disk.ApplyRecoveryPageStates(states);
    Check(disk.IsPageAllocated(page0) && disk.ReadPage(page0).data == after.data,
          "UNDO PAGE_FREE did not restore allocation and image");
}

void CreateTable(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER, data VARCHAR(2000))");
    database->Close();
}

void TestCommittedRedo(const std::filesystem::path& path) {
    CreateTable(path);
    {
        auto database = Database::Open(path, 8);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'committed')");
        // Simulated crash: COMMIT is durable, while the dirty table page is not flushed.
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        const auto rows = sql.ExecuteSQL("SELECT * FROM t").rows;
        Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(1),
              "Recovery did not REDO committed dirty data");
        database->Close();
    }
}

void TestLoserUndoAndTruncation(const std::filesystem::path& path) {
    CreateTable(path);
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("BEGIN");
        for (int id = 0; id < 4; ++id) {
            sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(id) + ", '" +
                           std::string(1500, 'z') + "')");
        }
        // Force loser pages and allocation metadata to disk, then omit ROLLBACK.
        database->Flush();
    }
    auto wal_path = path;
    wal_path.replace_extension(".wal");
    const auto complete_size = std::filesystem::file_size(wal_path);
    {
        std::ofstream output(wal_path, std::ios::binary | std::ios::app);
        output.write("partial", 7);
    }
    {
        auto database = Database::Open(path, 1);
        Check(std::filesystem::file_size(wal_path) != complete_size + 7 &&
              database->GetLogManager().GetRecords().back().GetType() == LogRecordType::Abort,
              "Recovery did not discard a truncated tail and complete loser undo");
        SqlEngine sql(database->GetCatalog());
        Check(sql.ExecuteSQL("SELECT * FROM t").rows.empty(),
              "Recovery did not UNDO loser table pages");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        Check(sql.ExecuteSQL("SELECT * FROM t").rows.empty(),
              "Repeated recovery was not idempotent");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-recovery-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestPhysicalRecovery(directory);
        TestCommittedRedo(directory / "committed.udb");
        TestLoserUndoAndTruncation(directory / "loser.udb");
        std::cout << "Crash recovery tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
