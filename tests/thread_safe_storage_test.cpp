#include "udb/buffer_pool_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::int64_t ReadCounter(const Page& page) {
    std::int64_t value = 0;
    std::memcpy(&value, page.data.data(), sizeof(value));
    return value;
}

void WriteCounter(Page& page, std::int64_t value) {
    std::memcpy(page.data.data(), &value, sizeof(value));
}

void TestConcurrentDisk(const std::filesystem::path& path) {
    DiskManager disk(path);
    constexpr int kThreads = 6;
    constexpr int kPagesPerThread = 20;
    std::mutex ids_mutex;
    std::atomic<bool> failed = false;
    std::vector<page_id_t> ids;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                std::vector<page_id_t> local;
                for (int i = 0; i < kPagesPerThread; ++i) {
                    const auto id = disk.AllocatePage();
                    Page page;
                    page.data.fill(static_cast<char>('a' + worker));
                    disk.WritePage(id, page);
                    if (disk.ReadPage(id).data != page.data) { failed = true; }
                    local.push_back(id);
                }
                const std::lock_guard<std::mutex> lock(ids_mutex);
                ids.insert(ids.end(), local.begin(), local.end());
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    std::sort(ids.begin(), ids.end());
    Check(!failed && ids.size() == kThreads * kPagesPerThread &&
          std::adjacent_find(ids.begin(), ids.end()) == ids.end() &&
          disk.GetPageCount() == static_cast<page_id_t>(ids.size()),
          "Concurrent DiskManager allocation produced duplicate page IDs");
}

void TestGuardLatchesAndConcurrentFlush(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 2);
    page_id_t page_id = -1;
    {
        auto page = pool.NewPageGuard();
        page_id = page.GetPageId();
        WriteCounter(page.GetPage(), 0);
    }

    constexpr int kWriters = 6;
    constexpr int kIncrements = 200;
    std::atomic<bool> failed = false;
    std::atomic<bool> running = true;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kWriters; ++worker) {
        workers.emplace_back([&] {
            try {
                for (int i = 0; i < kIncrements; ++i) {
                    auto page = pool.WritePage(page_id);
                    WriteCounter(page.GetPage(), ReadCounter(page.GetPage()) + 1);
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    std::thread reader([&] {
        try {
            while (running) {
                auto page = pool.ReadPage(page_id);
                const auto value = ReadCounter(page.GetPage());
                if (value < 0 || value > kWriters * kIncrements) { failed = true; }
            }
        } catch (...) {
            failed = true;
        }
    });
    std::thread flusher([&] {
        try {
            while (running) { pool.FlushAllPages(); }
        } catch (...) {
            failed = true;
        }
    });
    for (auto& worker : workers) { worker.join(); }
    running = false;
    reader.join();
    flusher.join();

    {
        auto page = pool.ReadPage(page_id);
        Check(ReadCounter(page.GetPage()) == kWriters * kIncrements,
              "Write page guards did not serialize page updates");
    }
    pool.FlushAllPages();
    Check(!failed && ReadCounter(disk.ReadPage(page_id)) == kWriters * kIncrements,
          "Concurrent pin/fetch/flush lost page data");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-thread-safe-storage-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestConcurrentDisk(directory / "disk.udb");
        TestGuardLatchesAndConcurrentFlush(directory / "pool.udb");
        std::cout << "Thread-safe storage tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
