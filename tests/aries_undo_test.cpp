#include "udb/recovery_manager.h"
#include "udb/buffer_pool_manager.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

Page Filled(char value) {
    Page page;
    page.data.fill(value);
    return page;
}

void TestUndoAndRestart(const std::filesystem::path& directory) {
    const auto data_path = directory / "undo.udb";
    const auto wal_path = directory / "undo.wal";
    const auto original = Filled('a');
    const auto committed = Filled('c');
    const auto loser = Filled('l');
    const auto freed = Filled('f');
    DiskManager disk(data_path);
    const auto page0 = disk.AllocatePage();
    const auto page1 = disk.AllocatePage();
    const auto page2 = disk.AllocatePage();
    disk.WritePage(page0, original);
    disk.WritePage(page1, Page{});
    disk.WritePage(page2, freed);

    lsn_t sequence_floor = 0;
    {
        LogManager log(wal_path);
        log.Append(LogRecord::Begin(1));
        log.Append(LogRecord::PageWrite(1, page0, original, committed));
        log.Append(LogRecord::Commit(1, 7));
        log.Append(LogRecord::Begin(2));
        log.Append(LogRecord::PageWrite(2, page0, committed, loser));
        log.Append(LogRecord::Begin(3));
        log.Append(LogRecord::PageAllocate(3, page1));
        log.Append(LogRecord::PageWrite(3, page1, Page{}, Filled('x')));
        log.Append(LogRecord::Begin(4));
        log.Append(LogRecord::PageFree(4, page2, freed));
        log.Flush();

        const auto analysis = RecoveryManager::Analyze(log);
        Check(analysis.losers.size() == 3, "Analysis did not find all loser transactions");
        const auto redo = RecoveryManager::Redo(disk, log, analysis);
        Check(redo.redone != 0, "Redo did not repeat loser history before Undo");
        const auto undo = RecoveryManager::Undo(disk, log, analysis);
        Check(undo.undone == 4 && undo.compensation_records == 4 &&
              undo.completed_transactions == 3,
              "Undo did not compensate all loser actions");
        disk.ApplyRecoveryPageStates(undo.page_states);
        Check(disk.ReadPage(page0).data == committed.data,
              "Undo overwrote committed page contents");
        Check(!disk.IsPageAllocated(page1), "Undo did not free a loser allocation");
        Check(disk.IsPageAllocated(page2) && disk.ReadPage(page2).data == freed.data,
              "Undo did not restore a loser free");
        Check(RecoveryManager::Analyze(log).losers.empty(),
              "Undo did not terminate every loser transaction");

        const auto second = RecoveryManager::Recover(disk, log);
        disk.ApplyRecoveryPageStates(second);
        Check(disk.ReadPage(page0).data == committed.data &&
              !disk.IsPageAllocated(page1) && disk.IsPageAllocated(page2),
              "Repeated recovery was not idempotent");

        sequence_floor = log.GetNextLsn();
        log.Reset();
        Check(log.GetNextLsn() == sequence_floor,
              "WAL reset reused a persistent page LSN");
        Check(log.Append(LogRecord::Begin(5)) == sequence_floor,
              "WAL did not continue at its durable sequence floor");
        log.Append(LogRecord::Commit(5, 8));
        log.Flush();
    }
    {
        LogManager reopened(wal_path);
        Check(!reopened.GetRecords().empty() &&
              reopened.GetRecords().front().GetLsn() == sequence_floor,
              "WAL sequence floor did not survive reopen");
    }
}

void TestCompletedAbortRedo(const std::filesystem::path& directory) {
    DiskManager disk(directory / "abort.udb");
    const auto page_id = disk.AllocatePage();
    const auto before = Filled('a');
    const auto after = Filled('b');
    disk.WritePage(page_id, before);
    LogManager log(directory / "abort.wal");
    BufferPoolManager pool(disk, 1, &log);
    TransactionManager transactions(&pool, &log);
    auto& transaction = transactions.Begin();
    pool.SetActiveTransaction(&transaction);
    {
        auto page = pool.WritePage(page_id);
        page.GetPage().data = after.data;
    }
    pool.SetActiveTransaction(nullptr);
    transactions.Abort(transaction);
    Check(log.GetRecords()[log.GetRecords().size() - 2].GetType() ==
              LogRecordType::Compensation,
          "Runtime abort did not log a CLR");

    // Replay the durable WAL against a stale crash image with no pageLSN.
    DiskManager recovery_disk(directory / "abort-recovery.udb");
    Check(recovery_disk.AllocatePage() == page_id, "Recovery page ID changed");
    recovery_disk.WritePage(page_id, after);
    const auto states = RecoveryManager::Recover(recovery_disk, log);
    recovery_disk.ApplyRecoveryPageStates(states);
    Check(recovery_disk.ReadPage(page_id).data == before.data,
          "Redo resurrected a completed aborted write");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-aries-undo-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestUndoAndRestart(directory);
        TestCompletedAbortRedo(directory);
        std::cout << "ARIES undo tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
