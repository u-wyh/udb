#include "udb/buffer_pool_manager.h"
#include "udb/transaction.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestDurablePageLsn(const std::filesystem::path& directory) {
    const auto data_path = directory / "durable.udb";
    const auto wal_path = directory / "durable.wal";
    page_id_t page_id = -1;
    lsn_t write_lsn = 0;
    {
        LogManager log(wal_path);
        DiskManager disk(data_path);
        BufferPoolManager pool(disk, 1, &log);
        TransactionManager transactions(&pool, &log);
        auto& transaction = transactions.Begin();
        pool.SetActiveTransaction(&transaction);
        {
            auto page = pool.NewPageGuard();
            page_id = page.GetPageId();
            page.GetPage().data[0] = 'p';
        }
        write_lsn = log.GetRecords().back().GetLsn();
        Check(!log.GetPersistentLsn() || *log.GetPersistentLsn() < write_lsn,
              "Page update became durable before its flush boundary");
        pool.FlushAllPages();
        Check(log.GetPersistentLsn() && *log.GetPersistentLsn() >= write_lsn &&
                  disk.GetPageLsn(page_id) == write_lsn &&
                  disk.ReadPage(page_id).data[0] == 'p',
              "WAL/data/pageLSN durable order is incorrect");
        pool.SetActiveTransaction(nullptr);
        transactions.Commit(transaction);
    }
    {
        DiskManager disk(data_path);
        Check(disk.GetPageLsn(page_id) == write_lsn &&
                  disk.ReadPage(page_id).data[0] == 'p',
              "PageLSN sidecar did not survive reopen");
    }
}

void TestLegacyAndVersioning(const std::filesystem::path& directory) {
    const auto data_path = directory / "legacy.udb";
    {
        DiskManager disk(data_path);
        const auto id = disk.AllocatePage();
        Page page;
        page.data[0] = 'l';
        disk.WritePage(id, page);
        disk.Sync();
    }
    const auto sidecar = DiskManager::GetPageLsnPath(data_path);
    Check(std::filesystem::remove(sidecar), "Cannot remove legacy fixture sidecar");
    {
        DiskManager disk(data_path);
        Check(!disk.GetPageLsn(0) && disk.ReadPage(0).data[0] == 'l',
              "Database without a pageLSN sidecar was not upgraded safely");
    }
    {
        std::fstream file(sidecar, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(8);
        const char unsupported[4] = {2, 0, 0, 0};
        file.write(unsupported, 4);
    }
    bool rejected = false;
    try { DiskManager disk(data_path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "Unsupported pageLSN sidecar version was accepted");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-page-lsn-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        TestDurablePageLsn(directory);
        TestLegacyAndVersioning(directory);
        std::cout << "Durable page LSN tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
