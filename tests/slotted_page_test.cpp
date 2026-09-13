#include "udb/slotted_page.h"
#include "udb/buffer_pool_manager.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

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
    const std::string bytes(size, value);
    return udb::Record(bytes.data(), bytes.size());
}

bool Equal(const udb::Record& a, const udb::Record& b) {
    return a.Size() == b.Size() &&
           (a.Size() == 0 || std::memcmp(a.Data(), b.Data(), a.Size()) == 0);
}

void TestRecords() {
    char bytes[] = {'a', '\0', 'b', static_cast<char>(0xff)};
    const udb::Record record(bytes, sizeof(bytes));
    bytes[0] = 'z';
    Check(record.Size() == sizeof(bytes) && record.Data()[0] == 'a' && record.Data()[1] == 0,
          "Record must own binary data");
    Check(udb::Record(nullptr, 0).Size() == 0, "Empty record failed");
    ExpectThrow<std::invalid_argument>([] { udb::Record record(nullptr, 1); });
    const udb::RID a{1, 2};
    const udb::RID b{1, 3};
    const udb::RID c{2, 0};
    Check(a == a && a != b && a < b && b < c && c > a && a <= b && c >= b,
          "RID comparison failed");
}

void TestLayout() {
    udb::Page page;
    ExpectThrow<std::invalid_argument>([&] { udb::SlottedPage invalid(page, -1); });
    udb::SlottedPage view(page, 7);
    ExpectThrow<std::runtime_error>([&] { view.GetFreeSpace(); });
    view.Init();
    const auto initial = udb::PAGE_SIZE - udb::SlottedPage::HEADER_SIZE - udb::SlottedPage::SLOT_SIZE;
    Check(view.GetFreeSpace() == initial && view.GetNextPageId() == -1, "Init failed");
    const char raw[] = {'a', '\0', 'b', '\0', 'c'};
    const udb::Record binary(raw, sizeof(raw));
    const auto a = view.InsertRecord(binary).value();
    const auto b = view.InsertRecord(Bytes(93, 'b')).value();
    const auto c = view.InsertRecord(Bytes(11, 'c')).value();
    Check(a == udb::RID{7, 0} && b == udb::RID{7, 1} && c == udb::RID{7, 2}, "RID allocation failed");
    Check(Equal(view.GetRecord(a), binary), "Binary record roundtrip failed");
    const auto free_before = view.GetFreeSpace();
    view.DeleteRecord(b);
    Check(view.GetFreeSpace() == free_before + 93, "Delete did not reclaim payload");
    Check(Equal(view.GetRecord(a), binary) && Equal(view.GetRecord(c), Bytes(11, 'c')),
          "Compaction changed surviving RIDs");
    const auto d = view.InsertRecord(Bytes(87, 'd')).value();
    Check(d.slot_id == 3 &&
              view.GetFreeSpace() == free_before + 93 - 87 - udb::SlottedPage::SLOT_SIZE,
          "Reclaimed space not reused");
    ExpectThrow<std::out_of_range>([&] { view.GetRecord(b); });
    ExpectThrow<std::out_of_range>([&] { view.DeleteRecord(b); });
    for (const auto rid : {udb::RID{-1, 0}, udb::RID{8, 0},
                           udb::RID{7, std::numeric_limits<udb::slot_id_t>::max()}}) {
        const auto before = page;
        ExpectThrow<std::out_of_range>([&] { view.GetRecord(rid); });
        ExpectThrow<std::out_of_range>([&] { view.DeleteRecord(rid); });
        Check(page.data == before.data, "Invalid RID mutated page");
    }
    for (const auto next : {udb::page_id_t{0}, udb::page_id_t{19},
                            std::numeric_limits<udb::page_id_t>::max(), udb::page_id_t{-1}}) {
        view.SetNextPageId(next);
        Check(view.GetNextPageId() == next, "Next page ID roundtrip failed");
    }
    ExpectThrow<std::invalid_argument>([&] { view.SetNextPageId(-2); });
    view.DeleteRecord(a);
    view.DeleteRecord(d);
    Check(Equal(view.GetRecord(c), Bytes(11, 'c')), "First/last deletion corrupted survivor");
    view.DeleteRecord(c);
    const auto empty = view.InsertRecord(udb::Record{}).value();
    Check(view.GetRecord(empty).Size() == 0, "Empty record roundtrip failed");
    view.DeleteRecord(empty);
}

void TestFull() {
    udb::Page page;
    udb::SlottedPage view(page, 0);
    view.Init();
    auto snapshot = page;
    Check(!view.InsertRecord(Bytes(udb::PAGE_SIZE + 1, 'x')), "Oversize record accepted");
    Check(page.data == snapshot.data, "Failed insert mutated page");
    const auto maximum = view.GetFreeSpace();
    const auto full = view.InsertRecord(Bytes(maximum, 'f')).value();
    Check(view.GetFreeSpace() == 0, "Exact fit left payload space");
    snapshot = page;
    Check(!view.InsertRecord(udb::Record{}) && page.data == snapshot.data, "Full page accepted slot");
    view.DeleteRecord(full);
    Check(view.InsertRecord(Bytes(maximum - udb::SlottedPage::SLOT_SIZE, 'g')).has_value(),
          "Full page deletion space not reusable");
    view.Init();
    std::vector<udb::RID> rids;
    const auto record = Bytes(37, 'r');
    while (const auto rid = view.InsertRecord(record)) {
        rids.push_back(*rid);
    }
    Check(!rids.empty() && rids.size() < udb::PAGE_SIZE, "Fill loop failed");
    snapshot = page;
    Check(!view.InsertRecord(record) && snapshot.data == page.data, "Full insert not atomic");
    for (const auto rid : rids) {
        Check(Equal(view.GetRecord(rid), record), "Fill changed prior record");
    }
    // Empty payloads still consume slots; directory exhaustion is bounded.
    view.Init();
    std::size_t slots = 0;
    while (view.InsertRecord(udb::Record{})) {
        ++slots;
    }
    Check(slots == (udb::PAGE_SIZE - udb::SlottedPage::HEADER_SIZE) / udb::SlottedPage::SLOT_SIZE,
          "Directory capacity incorrect");
}

void TestCorruption() {
    udb::Page valid;
    udb::SlottedPage original(valid, 0);
    original.Init();
    original.InsertRecord(Bytes(20, 'x'));
    // Corrupt magic, count, free boundary, next ID, offset, size and valid flag.
    for (const std::size_t position : {0U, 5U, 6U, 15U, 16U, 18U, 20U}) {
        auto damaged = valid;
        damaged.data[position] = static_cast<char>(0xfe);
        const auto before = damaged;
        udb::SlottedPage view(damaged, 0);
        ExpectThrow<std::runtime_error>([&] { view.GetFreeSpace(); });
        ExpectThrow<std::runtime_error>([&] { view.GetRecord({0, 0}); });
        ExpectThrow<std::runtime_error>([&] { view.DeleteRecord({0, 0}); });
        ExpectThrow<std::runtime_error>([&] { view.InsertRecord(Bytes(1, 'a')); });
        Check(damaged.data == before.data, "Corrupt layout was mutated");
    }
}

void TestModel() {
    udb::Page page;
    udb::SlottedPage view(page, 3);
    view.Init();
    std::map<udb::RID, udb::Record> live;
    std::vector<udb::RID> deleted;
    std::mt19937 random(2026);
    for (int step = 0; step < 600; ++step) {
        if (!live.empty() && random() % 3 == 0) {
            auto item = live.begin();
            std::advance(item, random() % live.size());
            view.DeleteRecord(item->first);
            deleted.push_back(item->first);
            live.erase(item);
        } else {
            const auto record = Bytes(random() % 150, static_cast<char>(random() % 128));
            const auto before = page;
            if (const auto rid = view.InsertRecord(record)) {
                Check(live.emplace(*rid, record).second, "Duplicate live RID");
            } else {
                Check(page.data == before.data, "Failed model insert mutated page");
            }
        }
        for (const auto& entry : live) {
            Check(Equal(view.GetRecord(entry.first), entry.second), "Model data mismatch");
        }
    }
    for (const auto rid : deleted) {
        ExpectThrow<std::out_of_range>([&] { view.GetRecord(rid); });
    }
}

void TestPersistence() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("udb-slotted-" + std::to_string(stamp));
    Check(std::filesystem::create_directory(directory), "Cannot create test directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto path = directory / "database.udb";
    udb::RID survivor;
    udb::RID deleted;
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        const auto [id, page] = pool.NewPage();
        udb::SlottedPage view(*page, id);
        view.Init();
        deleted = view.InsertRecord(Bytes(100, 'd')).value();
        survivor = view.InsertRecord(Bytes(200, 's')).value();
        view.DeleteRecord(deleted);
        view.SetNextPageId(23);
        pool.UnpinPage(id, true);
        pool.FlushAllPages();
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        auto* page = pool.FetchPage(survivor.page_id);
        udb::SlottedPage view(*page, survivor.page_id);
        Check(Equal(view.GetRecord(survivor), Bytes(200, 's')), "Reopen lost slotted record");
        Check(view.GetNextPageId() == 23, "Reopen lost next page ID");
        ExpectThrow<std::out_of_range>([&] { view.GetRecord(deleted); });
        pool.UnpinPage(survivor.page_id, false);
    }
}

}  // namespace

int main() {
    try {
        TestRecords();
        TestLayout();
        TestFull();
        TestCorruption();
        TestModel();
        TestPersistence();
        std::cout << "Slotted page tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
