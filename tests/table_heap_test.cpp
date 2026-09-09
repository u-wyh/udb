#include "udb/table_heap.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void ExpectThrow(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

udb::Record Bytes(std::size_t size, char value) {
    std::string bytes(size, value);
    if (size > 1) {
        bytes[1] = 0;
    }
    return udb::Record(bytes.data(), bytes.size());
}

bool Equal(const udb::Record& a, const udb::Record& b) {
    return a.Size() == b.Size() &&
           (a.Size() == 0 || std::memcmp(a.Data(), b.Data(), a.Size()) == 0);
}

void CheckUnpinned(udb::BufferPoolManager& pool, udb::page_id_t id) {
    pool.FetchPage(id);
    pool.UnpinPage(id, false);
    ExpectThrow<std::logic_error>([&] { pool.UnpinPage(id, false); });
}

std::vector<udb::RID> Scan(const udb::TableHeap& table) {
    std::vector<udb::RID> result;
    for (auto rid = table.GetFirstRID(); rid; rid = table.GetNextRID(*rid)) {
        Check(result.size() < 1000, "Scan did not terminate");
        result.push_back(*rid);
    }
    return result;
}

void Verify(const udb::TableHeap& table, const std::map<udb::RID, udb::Record>& live) {
    std::vector<udb::RID> expected;
    for (const auto& entry : live) {
        expected.push_back(entry.first);
        Check(Equal(table.GetRecord(entry.first), entry.second), "Record data mismatch");
    }
    Check(Scan(table) == expected, "Scan order/content mismatch");
}

void TestSinglePage(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    udb::BufferPoolManager pool(disk, 1);
    udb::TableHeap table(pool);
    const auto first = table.GetFirstPageId();
    Check(first >= 0 && !table.GetFirstRID(), "New table must be valid and empty");
    CheckUnpinned(pool, first);
    std::map<udb::RID, udb::Record> live;
    for (int i = 0; i < 8; ++i) {
        const auto record = Bytes(static_cast<std::size_t>(i * 7), static_cast<char>('a' + i));
        const auto rid = table.InsertRecord(record);
        Check(rid.page_id == first, "Small records should share a page");
        live.emplace(rid, record);
        CheckUnpinned(pool, first);
    }
    Verify(table, live);
    const auto removed = std::next(live.begin(), 3)->first;
    table.DeleteRecord(removed);
    live.erase(removed);
    ExpectThrow<std::out_of_range>([&] { table.GetRecord(removed); });
    ExpectThrow<std::out_of_range>([&] { table.DeleteRecord(removed); });
    ExpectThrow<std::out_of_range>([&] { table.GetNextRID(removed); });
    for (const auto rid : {udb::RID{-1, 0}, udb::RID{first, 65535}, udb::RID{999, 0}}) {
        ExpectThrow<std::out_of_range>([&] { table.GetRecord(rid); });
        ExpectThrow<std::out_of_range>([&] { table.DeleteRecord(rid); });
        ExpectThrow<std::out_of_range>([&] { table.GetNextRID(rid); });
    }
    CheckUnpinned(pool, first);
    Verify(table, live);
    for (const auto& entry : live) {
        table.DeleteRecord(entry.first);
    }
    Check(!table.GetFirstRID(), "Deleted table should scan empty");
    // TableHeap itself must not flush modified resident data.
    Check(disk.ReadPage(first).data == udb::Page{}.data, "Table unexpectedly flushed");
    udb::TableHeap other(pool);
    const auto foreign = other.InsertRecord(Bytes(10, 'f'));
    ExpectThrow<std::out_of_range>([&] { table.GetRecord(foreign); });
    ExpectThrow<std::out_of_range>([&] { table.DeleteRecord(foreign); });
}

void TestGrowthAndReopen(const std::filesystem::path& path) {
    udb::page_id_t first;
    std::map<udb::RID, udb::Record> live;
    std::vector<udb::RID> deleted;
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap table(pool);
        first = table.GetFirstPageId();
        for (int i = 0; i < 18; ++i) {
            const auto record = Bytes(1300, static_cast<char>('a' + i));
            live.emplace(table.InsertRecord(record), record);
        }
        Verify(table, live);
        std::vector<udb::page_id_t> chain;
        for (auto id = first; id != -1;) {
            Check(chain.size() < 20, "Page chain cycle");
            chain.push_back(id);
            auto* page = pool.FetchPage(id);
            udb::SlottedPage view(*page, id);
            const auto next = view.GetNextPageId();
            pool.UnpinPage(id, false);
            CheckUnpinned(pool, id);
            Check(next == -1 || next > id, "Chain is not append-only");
            id = next;
        }
        Check(chain.size() == 6, "Growth should produce six pages");
        // Empty the first, an intermediate, and the last page.
        for (auto item = live.begin(); item != live.end();) {
            if (item->first.page_id == chain[0] || item->first.page_id == chain[2] ||
                item->first.page_id == chain[5]) {
                deleted.push_back(item->first);
                table.DeleteRecord(item->first);
                item = live.erase(item);
            } else {
                ++item;
            }
        }
        Verify(table, live);
        const auto record = Bytes(1000, 'z');
        const auto reused = table.InsertRecord(record);
        Check(reused.page_id == first, "Insert must search free space from first page");
        live.emplace(reused, record);
        Verify(table, live);
        for (const auto id : chain) {
            CheckUnpinned(pool, id);
        }
        pool.FlushAllPages();
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap table(pool, first);
        Verify(table, live);
        for (const auto rid : deleted) {
            ExpectThrow<std::out_of_range>([&] { table.GetRecord(rid); });
        }
        const auto extra = Bytes(1400, 'n');
        live.emplace(table.InsertRecord(extra), extra);
        Verify(table, live);
        CheckUnpinned(pool, first);
    }
}

void TestFailures(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    udb::BufferPoolManager pool(disk, 1);
    ExpectThrow<std::invalid_argument>([&] { udb::TableHeap table(pool, -1); });
    ExpectThrow<std::out_of_range>([&] { udb::TableHeap table(pool, 123); });
    udb::TableHeap table(pool);
    const auto first = table.GetFirstPageId();
    const auto record = Bytes(udb::PAGE_SIZE - udb::SlottedPage::HEADER_SIZE - udb::SlottedPage::SLOT_SIZE, 'm');
    const auto rid = table.InsertRecord(record);
    const auto size = std::filesystem::file_size(path);
    ExpectThrow<std::length_error>([&] { table.InsertRecord(Bytes(udb::PAGE_SIZE, 'x')); });
    Check(std::filesystem::file_size(path) == size, "Oversize insert allocated page");
    auto* pinned = pool.FetchPage(first);
    const auto before = *pinned;
    ExpectThrow<std::runtime_error>([&] { table.InsertRecord(Bytes(1, 'x')); });
    ExpectThrow<std::runtime_error>([&] { udb::TableHeap blocked(pool); });
    Check(pinned->data == before.data && std::filesystem::file_size(path) == size,
          "All-pinned append failure changed chain or file");
    pool.UnpinPage(first, false);
    CheckUnpinned(pool, first);
    Check(Equal(table.GetRecord(rid), record), "Append failure lost existing record");
    // Inject malformed layout and cycles through the pool, then ensure every
    // exception releases its own pin so restoring the page remains possible.
    for (int fault = 0; fault < 3; ++fault) {
        auto* page = pool.FetchPage(first);
        const auto saved = *page;
        if (fault == 0) {
            page->data[0] = 0;
        } else {
            udb::SlottedPage(*page, first).SetNextPageId(fault == 1 ? first : 123);
        }
        pool.UnpinPage(first, true);
        ExpectThrow<std::exception>([&] { udb::TableHeap reopened(pool, first); });
        if (fault != 2) {
            ExpectThrow<std::runtime_error>([&] { table.GetRecord(rid); });
            ExpectThrow<std::runtime_error>([&] { table.DeleteRecord(rid); });
            ExpectThrow<std::runtime_error>([&] { table.GetFirstRID(); });
            ExpectThrow<std::runtime_error>([&] { table.InsertRecord(Bytes(1, 'x')); });
        }
        CheckUnpinned(pool, first);
        page = pool.FetchPage(first);
        *page = saved;
        pool.UnpinPage(first, true);
    }
    Check(Equal(table.GetRecord(rid), record), "Failure recovery lost record");
    const auto second = table.InsertRecord(Bytes(1, 's'));
    Check(second.page_id > first, "Append after failure did not recover");
    CheckUnpinned(pool, second.page_id);
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-table-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSinglePage(directory / "single.udb");
        TestGrowthAndReopen(directory / "growth.udb");
        TestFailures(directory / "failure.udb");
        std::cout << "Table heap tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
