#include "udb/recovery_manager.h"
#include "udb/table_heap.h"
#include "udb/transaction.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace udb;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

Record Text(const char* value) { return Record(value, 4); }

void CheckText(const TableHeap& heap, RID rid, const char* value, const char* message) {
    const auto record = heap.GetRecord(rid);
    Check(record.Size() == 4 && std::equal(record.Data(), record.Data() + 4, value), message);
}

void TestSamePageOwnership(const std::filesystem::path& data_path,
                           const std::filesystem::path& wal_path) {
    page_id_t first_page_id = -1;
    RID first_rid;
    RID second_rid;
    {
        LogManager log(wal_path);
        DiskManager disk(data_path);
        BufferPoolManager pool(disk, 2, &log);
        TableHeap heap(pool);
        first_page_id = heap.GetFirstPageId();
        first_rid = heap.InsertRecord(Text("row0"));
        second_rid = heap.InsertRecord(Text("row1"));
        Check(first_rid.page_id == second_rid.page_id,
              "Test records did not share one physical page");
        pool.FlushAllPages();

        TransactionManager transactions(&pool, &log);
        auto& aborted_first = transactions.Begin();
        auto& committed_second = transactions.Begin();
        pool.SetActiveTransaction(&aborted_first);
        Check(heap.UpdateRecord(first_rid, Text("a000")), "First same-page update failed");
        pool.SetActiveTransaction(nullptr);

        std::atomic<bool> started = false;
        std::atomic<bool> completed = false;
        std::atomic<bool> failed = false;
        std::thread second([&] {
            try {
                pool.SetActiveTransaction(&committed_second);
                started = true;
                Check(heap.UpdateRecord(second_rid, Text("b111")),
                      "Second same-page update failed");
                pool.SetActiveTransaction(nullptr);
                completed = true;
            } catch (...) {
                try { pool.SetActiveTransaction(nullptr); } catch (...) {}
                failed = true;
            }
        });
        while (!started.load()) { std::this_thread::yield(); }
        std::this_thread::sleep_for(30ms);
        Check(!completed && !failed,
              "Second transaction bypassed page lifetime ownership");
        transactions.Abort(aborted_first);
        second.join();
        Check(completed && !failed, "Second transaction did not resume after abort");
        transactions.Commit(committed_second);
        CheckText(heap, first_rid, "row0", "Abort overwrote the first RID incorrectly");
        CheckText(heap, second_rid, "b111", "Committed second RID was lost after abort");

        auto& committed_first = transactions.Begin();
        auto& aborted_second = transactions.Begin();
        pool.SetActiveTransaction(&committed_first);
        Check(heap.UpdateRecord(first_rid, Text("c000")), "Committed first update failed");
        pool.SetActiveTransaction(nullptr);
        completed = false;
        failed = false;
        std::thread later_abort([&] {
            try {
                pool.SetActiveTransaction(&aborted_second);
                Check(heap.UpdateRecord(second_rid, Text("d111")),
                      "Later aborted update failed");
                pool.SetActiveTransaction(nullptr);
                completed = true;
            } catch (...) {
                try { pool.SetActiveTransaction(nullptr); } catch (...) {}
                failed = true;
            }
        });
        std::this_thread::sleep_for(30ms);
        Check(!completed && !failed,
              "Later transaction wrote the page before the owner committed");
        transactions.Commit(committed_first);
        later_abort.join();
        Check(completed && !failed, "Later transaction did not resume after commit");
        transactions.Abort(aborted_second);
        CheckText(heap, first_rid, "c000", "Later abort erased an earlier committed RID");
        CheckText(heap, second_rid, "b111", "Later abort failed to restore its own RID");

        auto& crash_first = transactions.Begin();
        auto& crash_second = transactions.Begin();
        pool.SetActiveTransaction(&crash_first);
        Check(heap.UpdateRecord(first_rid, Text("e000")), "Crash first update failed");
        pool.SetActiveTransaction(nullptr);
        completed = false;
        failed = false;
        std::thread crash_waiter([&] {
            try {
                pool.SetActiveTransaction(&crash_second);
                Check(heap.UpdateRecord(second_rid, Text("f111")),
                      "Crash second update failed");
                pool.SetActiveTransaction(nullptr);
                completed = true;
            } catch (...) {
                try { pool.SetActiveTransaction(nullptr); } catch (...) {}
                failed = true;
            }
        });
        std::this_thread::sleep_for(30ms);
        Check(!completed && !failed, "Crash pair modified one page concurrently");
        transactions.Commit(crash_first);
        crash_waiter.join();
        Check(completed && !failed, "Crash second transaction did not resume");
        transactions.Commit(crash_second);
        // Simulated crash: committed WAL is durable, dirty data page is not flushed.
    }
    {
        LogManager log(wal_path);
        DiskManager disk(data_path);
        const auto states = RecoveryManager::Recover(disk, log);
        disk.ApplyRecoveryPageStates(states);
        BufferPoolManager pool(disk, 1, &log);
        TableHeap heap(pool, first_page_id);
        CheckText(heap, first_rid, "e000", "Recovery lost the first same-page commit");
        CheckText(heap, second_rid, "f111", "Recovery lost the second same-page commit");
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-same-page-transaction-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSamePageOwnership(directory / "database.udb", directory / "database.wal");
        std::cout << "Same-page transaction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
