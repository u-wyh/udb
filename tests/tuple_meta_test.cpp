#include "udb/buffer_pool_manager.h"
#include "udb/slotted_page.h"
#include "udb/table_heap.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
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

udb::Record Text(const std::string& value) {
    return udb::Record(value.data(), value.size());
}

bool Equal(const udb::Record& record, const std::string& value) {
    return record.Size() == value.size() &&
           (value.empty() || std::memcmp(record.Data(), value.data(), value.size()) == 0);
}

void Write(udb::Page& page, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        page.data[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

void TestPhysicalMetadata() {
    udb::Page page;
    udb::SlottedPage view(page, 7);
    view.Init();
    const auto first = view.InsertRecord(Text("alpha")).value();
    const auto second = view.InsertRecord(Text(std::string("b\0eta", 5)), {42, false}).value();
    Check(view.GetTupleMeta(first) == udb::TupleMeta{}, "Default tuple metadata is wrong");
    Check(view.GetTupleMeta(second) == udb::TupleMeta{42, false}, "Inserted tuple metadata is wrong");

    const auto bytes = view.GetRecord(second);
    view.SetTupleMeta(second, {99, true});
    Check(view.GetTupleMeta(second) == udb::TupleMeta{99, true}, "Tuple metadata update failed");
    const auto unchanged = view.GetRecord(second);
    Check(bytes.Size() == unchanged.Size() &&
              std::memcmp(bytes.Data(), unchanged.Data(), bytes.Size()) == 0,
          "Tuple metadata changed logical record bytes");
    Check(view.UpdateRecord(second, Text("updated")), "Record update failed");
    Check(view.GetTupleMeta(second) == udb::TupleMeta{99, true}, "Record update lost tuple metadata");
    view.DeleteRecord(second);
    ExpectThrow<std::out_of_range>([&] { view.GetTupleMeta(second); });
}

void TestLegacyCompatibility() {
    udb::Page page;
    constexpr std::size_t header = 16;
    constexpr std::size_t old_slot = 6;
    constexpr std::uint64_t old_magic = 0x31504455;
    const std::string first = "old";
    const std::string second = "v1";
    const auto first_offset = udb::PAGE_SIZE - first.size();
    const auto second_offset = first_offset - second.size();
    Write(page, 0, 4, old_magic);
    Write(page, 4, 2, 2);
    Write(page, 6, 2, second_offset);
    Write(page, 8, 8, ~std::uint64_t{0});
    Write(page, header, 2, first_offset);
    Write(page, header + 2, 2, first.size());
    Write(page, header + 4, 2, 1);
    Write(page, header + old_slot, 2, second_offset);
    Write(page, header + old_slot + 2, 2, second.size());
    Write(page, header + old_slot + 4, 2, 1);
    std::memcpy(page.data.data() + first_offset, first.data(), first.size());
    std::memcpy(page.data.data() + second_offset, second.data(), second.size());

    udb::SlottedPage view(page, 11);
    Check(Equal(view.GetRecord({11, 0}), first) && Equal(view.GetRecord({11, 1}), second),
          "Legacy page records are not readable");
    Check(view.GetTupleMeta({11, 0}) == udb::TupleMeta{},
          "Legacy tuple must expose default metadata");
    view.SetTupleMeta({11, 1}, {7, true});
    Check(view.GetTupleMeta({11, 1}) == udb::TupleMeta{7, true},
          "Legacy page metadata upgrade failed");
    Check(Equal(view.GetRecord({11, 0}), first) && Equal(view.GetRecord({11, 1}), second),
          "Legacy upgrade changed records or RIDs");

    udb::Page full;
    const auto payload_begin = header + old_slot;
    Write(full, 0, 4, old_magic);
    Write(full, 4, 2, 1);
    Write(full, 6, 2, payload_begin);
    Write(full, 8, 8, ~std::uint64_t{0});
    Write(full, header, 2, payload_begin);
    Write(full, header + 2, 2, udb::PAGE_SIZE - payload_begin);
    Write(full, header + 4, 2, 1);
    std::memset(full.data.data() + payload_begin, 'x', udb::PAGE_SIZE - payload_begin);
    const auto snapshot = full;
    udb::SlottedPage full_view(full, 12);
    Check(!full_view.InsertRecord(udb::Record{}, {1, false}) && full.data == snapshot.data,
          "Failed legacy metadata insert changed a full page");
    ExpectThrow<std::length_error>([&] { full_view.SetTupleMeta({12, 0}, {1, false}); });
    Check(full.data == snapshot.data, "Failed legacy metadata upgrade changed a full page");
}

void TestPersistence() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("udb-tuple-meta-" + std::to_string(stamp));
    Check(std::filesystem::create_directory(directory), "Cannot create test directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto path = directory / "data.udb";
    udb::page_id_t first_page = -1;
    udb::RID rid;
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap heap(pool);
        first_page = heap.GetFirstPageId();
        rid = heap.InsertRecord(Text("persistent"), {1234, true});
        Check(heap.GetTupleMeta(rid) == udb::TupleMeta{1234, true},
              "TableHeap metadata read failed");
        heap.SetTupleMeta(rid, {5678, false});
        pool.FlushAllPages();
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap heap(pool, first_page);
        Check(Equal(heap.GetRecord(rid), "persistent"), "Reopen lost record bytes");
        Check(heap.GetTupleMeta(rid) == udb::TupleMeta{5678, false},
              "Reopen lost tuple metadata");
    }
}

}  // namespace

int main() {
    try {
        TestPhysicalMetadata();
        TestLegacyCompatibility();
        TestPersistence();
        std::cout << "Tuple metadata tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
