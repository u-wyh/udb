#include "udb/catalog.h"

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

Record IntegerRecord(std::int64_t value) {
    std::array<char, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return Record(bytes.data(), bytes.size());
}

std::int64_t RecordInteger(const Record& record) {
    Check(record.Size() == sizeof(std::int64_t), "Unexpected concurrent record size");
    std::int64_t value = 0;
    std::memcpy(&value, record.Data(), sizeof(value));
    return value;
}

void TestConcurrentTableHeap(BufferPoolManager& pool) {
    TableHeap heap(pool);
    constexpr int kThreads = 6;
    constexpr int kRows = 80;
    std::atomic<bool> failed = false;
    std::mutex result_mutex;
    std::vector<std::pair<RID, std::int64_t>> inserted;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                std::vector<std::pair<RID, std::int64_t>> local;
                for (int row = 0; row < kRows; ++row) {
                    const auto value = static_cast<std::int64_t>(worker * kRows + row);
                    local.emplace_back(heap.InsertRecord(IntegerRecord(value)), value);
                }
                const std::lock_guard<std::mutex> lock(result_mutex);
                inserted.insert(inserted.end(), local.begin(), local.end());
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    Check(!failed && inserted.size() == kThreads * kRows,
          "Concurrent TableHeap inserts failed");

    workers.clear();
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (std::size_t i = static_cast<std::size_t>(worker);
                     i < inserted.size(); i += kThreads) {
                    if (RecordInteger(heap.GetRecord(inserted[i].first)) != inserted[i].second) {
                        failed = true;
                    }
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    std::size_t scanned = 0;
    for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) { ++scanned; }
    Check(!failed && scanned == inserted.size(),
          "Concurrent TableHeap reads or page-chain growth lost rows");
}

void TestConcurrentBPlusTree(BufferPoolManager& pool) {
    BPlusTreeOptions options;
    options.leaf_max_size = 4;
    options.internal_max_size = 4;
    BPlusTree tree(pool, options);
    constexpr int kKeys = 600;
    constexpr int kThreads = 6;
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (int key = worker; key < kKeys; key += kThreads) {
                    if (!tree.Insert(key, RID{key, 0})) { failed = true; }
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    tree.Validate();

    workers.clear();
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (int key = worker; key < kKeys; key += kThreads) {
                    const auto rid = tree.GetValue(key);
                    if (!rid || rid->page_id != key) { failed = true; }
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }

    workers.clear();
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (int key = 2 * worker + 1; key < kKeys; key += 2 * kThreads) {
                    if (!tree.Remove(key)) { failed = true; }
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    tree.Validate();
    Check(!failed, "Concurrent B+ tree insert/read/remove failed");
    for (int key = 0; key < kKeys; ++key) {
        Check(tree.GetValue(key).has_value() == (key % 2 == 0),
              "Concurrent B+ tree result is inconsistent");
    }
}

void TestConcurrentCatalog(BufferPoolManager& pool) {
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER)});
    constexpr int kTables = 24;
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    for (int table = 0; table < kTables; ++table) {
        workers.emplace_back([&, table] {
            try {
                const auto name = "table_" + std::to_string(table);
                const auto& metadata = catalog.CreateTable(name, schema);
                if (metadata.GetTableName() != name) { failed = true; }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    const auto ids = catalog.ListTables();
    Check(!failed && ids.size() == kTables &&
          std::adjacent_find(ids.begin(), ids.end(),
                             [](table_id_t left, table_id_t right) { return left >= right; }) == ids.end(),
          "Concurrent Catalog publication produced invalid IDs");
    for (int table = 0; table < kTables; ++table) {
        const auto& found = catalog.GetTable("table_" + std::to_string(table)).GetSchema();
        Check(found.GetColumnCount() == 1 && found.GetColumn(0).GetName() == "id" &&
              found.GetColumn(0).GetType() == TypeId::INTEGER,
              "Concurrent Catalog lookup returned wrong metadata");
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-concurrent-table-index-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        DiskManager disk(directory / "database.udb");
        BufferPoolManager pool(disk, 8);
        TestConcurrentTableHeap(pool);
        TestConcurrentBPlusTree(pool);
        TestConcurrentCatalog(pool);
        std::cout << "Concurrent table/index tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
