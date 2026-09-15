#include "udb/recovery_manager.h"

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

std::size_t CountClr(const LogManager& log) {
    std::size_t count = 0;
    for (const auto& record : log.GetRecords()) {
        if (record.GetType() == LogRecordType::Compensation) { ++count; }
    }
    return count;
}

void TestCrashAfterClrFlush(const std::filesystem::path& directory) {
    const auto data_path = directory / "before-page-write.udb";
    const auto wal_path = directory / "before-page-write.wal";
    lsn_t first_write = 0;
    {
        DiskManager disk(data_path);
        const auto page_id = disk.AllocatePage();
        disk.WritePage(page_id, Filled('a'));
        LogManager log(wal_path);
        log.Append(LogRecord::Begin(1));
        first_write = log.Append(LogRecord::PageWrite(
            1, page_id, Filled('a'), Filled('b')));
        log.Append(LogRecord::PageWrite(1, page_id, Filled('b'), Filled('c')));
        log.Flush();
        const auto analysis = RecoveryManager::Analyze(log);
        RecoveryManager::Redo(disk, log, analysis);
        // Crash point: the CLR is durable, but its page image is not installed.
        log.Append(LogRecord::Compensation(
            1, page_id, CompensationType::PageWrite, Filled('b'), first_write));
        log.Flush();
    }
    {
        DiskManager disk(data_path);
        LogManager log(wal_path);
        auto states = RecoveryManager::Recover(disk, log);
        disk.ApplyRecoveryPageStates(states);
        Check(disk.ReadPage(0).data == Filled('a').data,
              "Restart did not redo a durable CLR before continuing Undo");
        Check(CountClr(log) == 2 && RecoveryManager::Analyze(log).losers.empty(),
              "Restart repeated an already compensated action");
    }
}

void TestRepeatedCrashDuringUndo(const std::filesystem::path& directory) {
    const auto data_path = directory / "after-page-write.udb";
    const auto wal_path = directory / "after-page-write.wal";
    {
        DiskManager disk(data_path);
        const auto page_id = disk.AllocatePage();
        disk.WritePage(page_id, Filled('a'));
        LogManager log(wal_path);
        log.Append(LogRecord::Begin(2));
        log.Append(LogRecord::PageWrite(2, page_id, Filled('a'), Filled('b')));
        log.Append(LogRecord::PageWrite(2, page_id, Filled('b'), Filled('c')));
        log.Append(LogRecord::PageWrite(2, page_id, Filled('c'), Filled('d')));
        log.Flush();
        const auto analysis = RecoveryManager::Analyze(log);
        RecoveryManager::Redo(disk, log, analysis);
        const auto partial = RecoveryManager::Undo(disk, log, analysis, 1);
        Check(!partial.complete && partial.compensation_records == 1,
              "Bounded Undo did not stop at the crash boundary");
    }
    {
        DiskManager disk(data_path);
        LogManager log(wal_path);
        const auto analysis = RecoveryManager::Analyze(log);
        RecoveryManager::Redo(disk, log, analysis);
        const auto partial = RecoveryManager::Undo(disk, log, analysis, 1);
        Check(!partial.complete && CountClr(log) == 2,
              "Restart did not continue from CLR undoNextLSN");
    }
    {
        DiskManager disk(data_path);
        LogManager log(wal_path);
        auto states = RecoveryManager::Recover(disk, log);
        disk.ApplyRecoveryPageStates(states);
        Check(disk.ReadPage(0).data == Filled('a').data,
              "Repeated recovery crash did not reach the original image");
        Check(CountClr(log) == 3 && RecoveryManager::Analyze(log).losers.empty(),
              "Repeated recovery duplicated compensation or left a loser");
        states = RecoveryManager::Recover(disk, log);
        disk.ApplyRecoveryPageStates(states);
        Check(CountClr(log) == 3 && disk.ReadPage(0).data == Filled('a').data,
              "Completed restart recovery was not idempotent");
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-recovery-restart-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestCrashAfterClrFlush(directory);
        TestRepeatedCrashDuringUndo(directory);
        std::cout << "Recovery restart tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
