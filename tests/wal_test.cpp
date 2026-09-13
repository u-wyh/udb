#include "udb/database.h"
#include "udb/log_manager.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected error");
}

Page Filled(char value) {
    Page page;
    page.data.fill(value);
    return page;
}

void TestLogFormat(const std::filesystem::path& path) {
    const auto before = Filled('b');
    const auto after = Filled('a');
    {
        LogManager log(path);
        Check(log.GetRecords().empty() && log.GetNextLsn() == 0 && !log.GetPersistentLsn(),
              "New WAL state is wrong");
        Check(log.Append(LogRecord::Begin(7)) == 0, "BEGIN LSN is wrong");
        Check(log.Append(LogRecord::PageWrite(7, 3, before, after)) == 1,
              "PAGE_WRITE LSN is wrong");
        Check(log.Append(LogRecord::PageAllocate(7, 4)) == 2, "PAGE_ALLOC LSN is wrong");
        Check(log.Append(LogRecord::PageFree(7, 5, before)) == 3, "PAGE_FREE LSN is wrong");
        Check(log.Append(LogRecord::Commit(7)) == 4, "COMMIT LSN is wrong");
        Check(log.Append(LogRecord::Abort(8)) == 5, "ABORT LSN is wrong");
        Check(!log.GetPersistentLsn(), "Unflushed WAL was reported persistent");
        log.Flush();
        Check(log.GetPersistentLsn() == 5, "Flushed WAL boundary is wrong");
        Reject([&] { log.Append(LogRecord::PageAllocate(9, -1)); });
    }
    {
        LogManager log(path);
        const auto& records = log.GetRecords();
        Check(records.size() == 6 && log.GetNextLsn() == 6 && log.GetPersistentLsn() == 5,
              "Reopened WAL sequence is wrong");
        Check(records[0].GetType() == LogRecordType::Begin &&
              records[0].GetTransactionId() == 7 && !records[0].GetPageId(),
              "BEGIN record decoded incorrectly");
        Check(records[1].GetType() == LogRecordType::PageWrite &&
              records[1].GetPageId() == 3 && records[1].GetBeforeImage() &&
              records[1].GetAfterImage() && records[1].GetBeforeImage()->data == before.data &&
              records[1].GetAfterImage()->data == after.data,
              "PAGE_WRITE images decoded incorrectly");
        Check(records[2].GetType() == LogRecordType::PageAllocate &&
              records[2].GetPageId() == 4 && !records[2].GetBeforeImage(),
              "PAGE_ALLOC decoded incorrectly");
        Check(records[3].GetType() == LogRecordType::PageFree &&
              records[3].GetPageId() == 5 && records[3].GetBeforeImage() &&
              records[3].GetBeforeImage()->data == before.data && !records[3].GetAfterImage(),
              "PAGE_FREE decoded incorrectly");
        Check(records[4].GetType() == LogRecordType::Commit &&
              records[5].GetType() == LogRecordType::Abort,
              "Transaction completion records decoded incorrectly");
        Check(log.Append(LogRecord::Begin(9)) == 6, "Reopened WAL did not continue its LSN");
        log.Flush();
    }
}

void TestValidation(const std::filesystem::path& valid,
                    const std::filesystem::path& directory) {
    const auto corrupt = directory / "corrupt.wal";
    std::filesystem::copy_file(valid, corrupt);
    {
        std::fstream file(corrupt, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(100);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x5a;
        file.seekp(100);
        file.write(&byte, 1);
    }
    Reject([&] { LogManager log(corrupt); });

    const auto truncated = directory / "truncated.wal";
    std::filesystem::copy_file(valid, truncated);
    std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
    {
        LogManager log(truncated);
        Check(log.GetRecords().size() == 6 &&
              std::filesystem::file_size(truncated) < std::filesystem::file_size(valid),
              "Truncated WAL tail was not discarded at its last valid record");
    }
    Reject([&] { LogManager log(directory / "wrong.log"); });
}

void TestDatabaseWal(const std::filesystem::path& path) {
    auto wal_path = path;
    wal_path.replace_extension(".wal");
    {
        auto database = Database::Create(path, 1);
        Check(std::filesystem::is_regular_file(wal_path) &&
              database->GetLogManager().GetPath() == wal_path,
              "Database did not create its companion WAL");
        database->GetLogManager().Append(LogRecord::Begin(42));
        database->GetLogManager().Append(LogRecord::Commit(42));
        database->GetLogManager().Flush();
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        const auto& records = database->GetLogManager().GetRecords();
        Check(records.empty(), "Clean Database::Close did not reset its WAL");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-wal-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        const auto wal = directory / "records.wal";
        TestLogFormat(wal);
        TestValidation(wal, directory);
        TestDatabaseWal(directory / "database.udb");
        std::cout << "WAL foundation tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
