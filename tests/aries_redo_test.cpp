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

void TestRedoAndPageLsn(const std::filesystem::path& directory) {
    const auto data_path = directory / "redo.udb";
    const auto wal_path = directory / "redo.wal";
    DiskManager disk(data_path);
    const auto page0 = disk.AllocatePage();
    disk.WritePage(page0, Filled('a'), 1);

    LogManager log(wal_path);
    log.Append(LogRecord::Begin(1));
    log.Append(LogRecord::PageWrite(1, page0, Page{}, Filled('a'))); // durable already
    log.Append(LogRecord::Commit(1, 1));
    log.Append(LogRecord::Begin(2));
    log.Append(LogRecord::PageAllocate(2, 1));
    log.Append(LogRecord::PageWrite(2, 1, Page{}, Filled('b')));
    log.Append(LogRecord::Begin(3));
    log.Append(LogRecord::PageAllocate(3, 2));
    log.Append(LogRecord::PageFree(3, 2, Page{}));
    log.Append(LogRecord::Commit(3, 2));
    log.Flush();

    const auto analysis = RecoveryManager::Analyze(log);
    const auto first = RecoveryManager::Redo(disk, log, analysis);
    Check(first.redone == 4 && first.skipped_by_page_lsn == 1 &&
              disk.ReadPage(1).data[0] == 'b' && disk.GetPageLsn(1) == 5 &&
              first.page_states.at(1) && !first.page_states.at(2),
          "ARIES Redo did not repeat history correctly");
    const auto second = RecoveryManager::Redo(disk, log, analysis);
    Check(second.redone == 0 && second.skipped_by_page_lsn == 5 &&
              disk.ReadPage(1).data[0] == 'b',
          "ARIES Redo was not idempotent");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-aries-redo-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        TestRedoAndPageLsn(directory);
        std::cout << "ARIES redo tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
