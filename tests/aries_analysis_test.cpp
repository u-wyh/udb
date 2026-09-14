#include "udb/recovery_manager.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

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

void TestAnalysis(const std::filesystem::path& path) {
    LogManager log(path);
    const auto empty = Page{};
    log.Append(LogRecord::Begin(10));                       // 0
    log.Append(LogRecord::Begin(20));                       // 1
    log.Append(LogRecord::PageWrite(10, 4, empty, Filled('a'))); // 2
    log.Append(LogRecord::PageAllocate(20, 8));             // 3
    log.Append(LogRecord::Commit(10, 7));                   // 4
    log.Append(LogRecord::Begin(30));                       // 5
    log.Append(LogRecord::PageFree(30, 9, Filled('f')));    // 6
    log.Append(LogRecord::Abort(30));                       // 7
    log.Append(LogRecord::PageWrite(20, 4, Filled('a'), Filled('b'))); // 8
    log.Append(LogRecord::Compensation(20, 8, CompensationType::PageAllocate,
                                       Page{}, 1));          // 9
    log.Flush();

    const auto analysis = RecoveryManager::Analyze(log);
    Check(analysis.transaction_table.size() == 3 &&
              analysis.winners == std::set<transaction_id_t>{10} &&
              analysis.losers == std::set<transaction_id_t>{20} &&
              analysis.aborted == std::set<transaction_id_t>{30},
          "Analysis transaction classification is wrong");
    Check(analysis.transaction_table.at(10).state == RecoveryTransactionState::Committed &&
              analysis.transaction_table.at(10).last_lsn == 4 &&
              analysis.transaction_table.at(10).commit_timestamp == 7 &&
              analysis.transaction_table.at(20).last_lsn == 9 &&
              analysis.transaction_table.at(30).state == RecoveryTransactionState::Aborted,
          "Analysis transaction table is wrong");
    Check(analysis.dirty_page_table.size() == 3 &&
              analysis.dirty_page_table.at(4) == 2 &&
              analysis.dirty_page_table.at(8) == 3 &&
              analysis.dirty_page_table.at(9) == 6 &&
              analysis.redo_start_lsn == 2 && analysis.maximum_commit_timestamp == 7,
          "Analysis dirty page table or redo start is wrong");

    LogManager reopened(path);
    const auto repeated = RecoveryManager::Analyze(reopened);
    Check(repeated.losers == analysis.losers &&
              repeated.dirty_page_table == analysis.dirty_page_table,
          "Analysis changed after WAL reopen");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-aries-analysis-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        TestAnalysis(directory / "analysis.wal");
        std::cout << "ARIES analysis tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
