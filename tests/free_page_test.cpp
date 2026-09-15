#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

Page Pattern(char value) {
    Page page;
    page.data.fill(value);
    return page;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(static_cast<bool>(output), "Cannot write metadata fixture");
}

void Put(std::string& bytes, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

void TestDiskManager(const std::filesystem::path& path) {
    DiskManager disk(path);
    const auto a = disk.AllocatePage();
    const auto b = disk.AllocatePage();
    const auto c = disk.AllocatePage();
    Check(a == 0 && b == 1 && c == 2, "Initial page allocation is not monotonic");
    disk.WritePage(b, Pattern('x'));
    const auto size = std::filesystem::file_size(path);
    disk.DeallocatePage(b);
    Check(!disk.IsPageAllocated(b) && disk.GetFreePageIds() == std::set<page_id_t>{b},
          "Deallocated page was not tracked as free");
    Reject<std::out_of_range>([&] { disk.ReadPage(b); });
    Reject<std::out_of_range>([&] { disk.WritePage(b, Pattern('y')); });
    Reject<std::logic_error>([&] { disk.DeallocatePage(b); });
    Reject<std::out_of_range>([&] { disk.DeallocatePage(-1); });
    Reject<std::out_of_range>([&] { disk.DeallocatePage(3); });
    Check(disk.AllocatePage() == b && disk.ReadPage(b).data == Page{}.data,
          "Reallocated page ID/content is wrong");
    Check(std::filesystem::file_size(path) == size, "Page reuse changed file size");
}

void TestBufferPool(const std::filesystem::path& path) {
    DiskManager disk(path);
    const auto uncached = disk.AllocatePage();
    BufferPoolManager pool(disk, 1);
    Check(pool.DeletePage(uncached) && !disk.IsPageAllocated(uncached), "Uncached page deletion failed");
    const auto [id, page] = pool.NewPage();
    Check(id == uncached && page->data == Page{}.data, "NewPage did not reuse uncached deletion");
    *page = Pattern('d');
    Check(!pool.DeletePage(id) && disk.IsPageAllocated(id), "Pinned deletion changed state");
    pool.UnpinPage(id, true);
    Check(pool.DeletePage(id) && !disk.IsPageAllocated(id), "Unpinned resident deletion failed");
    Reject<std::out_of_range>([&] { pool.FlushPage(id); });
    pool.FlushAllPages();
    Reject<std::out_of_range>([&] { disk.ReadPage(id); });
    const auto [reused, clean] = pool.NewPage();
    Check(reused == id && clean == page && clean->data == Page{}.data,
          "Deleted frame/page was not reused cleanly");
    pool.UnpinPage(reused, false);
    pool.FlushAllPages();
    Check(disk.ReadPage(reused).data == Page{}.data, "Dirty deleted contents were written back");
}

void TestDropAndReuse(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("value", TypeId::VARCHAR, 2000)});
    const auto& victim = catalog.CreateTable("victim", schema);
    const auto victim_id = victim.GetTableId();
    std::set<page_id_t> victim_pages{victim.GetFirstPageId()};
    for (int i = 0; i < 7; ++i) {
        victim_pages.insert(catalog.GetTableHeap(victim_id).InsertRecord(
            Tuple(schema, {Value::Varchar(std::string(1400, static_cast<char>('a' + i)))})
                .Serialize(schema)).page_id);
    }
    const auto& other = catalog.CreateTable("other", schema);
    const auto other_id = other.GetTableId();
    const auto other_page = other.GetFirstPageId();
    const auto other_rid = catalog.GetTableHeap(other_id).InsertRecord(
        Tuple(schema, {Value::Varchar(std::string(1400, 'k'))}).Serialize(schema));
    const auto file_size = std::filesystem::file_size(path);

    pool.FetchPage(*victim_pages.begin());
    Reject<std::runtime_error>([&] { catalog.DropTable(victim_id); });
    Check(catalog.GetTable(victim_id).GetTableId() == victim_id, "Failed DROP removed metadata");
    for (const auto id : victim_pages) { Check(disk.IsPageAllocated(id), "Failed DROP released part of the chain"); }
    pool.UnpinPage(*victim_pages.begin(), false);

    catalog.DropTable(victim_id);
    for (const auto id : victim_pages) { Check(!disk.IsPageAllocated(id), "DROP did not release the full chain"); }
    Check(disk.IsPageAllocated(other_page) &&
          Tuple::Deserialize(catalog.GetTableHeap(other_id).GetRecord(other_rid), schema).GetValue(0) ==
              Value::Varchar(std::string(1400, 'k')), "DROP affected another table");
    const auto& recreated = catalog.CreateTable("victim", schema);
    Check(recreated.GetTableId() > other_id && recreated.GetFirstPageId() == *victim_pages.begin(),
          "Recreated table did not use a new table ID and the smallest free page");
    for (int i = 0; i < 4; ++i) {
        catalog.GetTableHeap(other_id).InsertRecord(
            Tuple(schema, {Value::Varchar(std::string(1400, static_cast<char>('p' + i)))})
                .Serialize(schema));
    }
    std::size_t other_count = 0;
    for (auto rid = catalog.GetTableHeap(other_id).GetFirstRID(); rid;
         rid = catalog.GetTableHeap(other_id).GetNextRID(*rid)) {
        ++other_count;
    }
    Check(other_count == 5, "Table chain failed after appending a lower reused page ID");
    catalog.DropTable(recreated.GetTableId());
    for (int i = 0; i < 5; ++i) {
        const auto& temporary = catalog.CreateTable("temporary", Schema({}));
        catalog.DropTable(temporary.GetTableId());
    }
    Check(std::filesystem::file_size(path) == file_size, "Repeated CREATE/DROP grew the data file");
}

void TestPersistence(const std::filesystem::path& directory) {
    const auto path = directory / "persistent.udb";
    page_id_t first_free;
    std::uintmax_t size;
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE victim (value VARCHAR(2000))");
        first_free = database->GetCatalog().GetTable("victim").GetFirstPageId();
        for (int i = 0; i < 7; ++i) {
            engine.ExecuteSQL("INSERT INTO victim VALUES ('" + std::string(1400, 'x') + "')");
        }
        engine.ExecuteSQL("CREATE TABLE keep (id INTEGER)");
        engine.ExecuteSQL("INSERT INTO keep VALUES (9)");
        size = std::filesystem::file_size(path);
        engine.ExecuteSQL("DROP TABLE victim");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        Check(engine.ExecuteSQL("SELECT * FROM keep").rows.at(0).GetValue(0) == Value::Integer(9),
              "Remaining table did not survive reopen");
        engine.ExecuteSQL("CREATE TABLE replacement (id INTEGER)");
        Check(database->GetCatalog().GetTable("replacement").GetFirstPageId() == first_free,
              "Persisted free page was not reused after reopen");
        Check(std::filesystem::file_size(path) == size, "Persistent free-page reuse grew the file");
        database->Close();
    }
}

void TestMetadataValidation(const std::filesystem::path& directory) {
    const auto path = directory / "metadata.udb";
    const auto meta = directory / "metadata.meta";
    {
        auto database = Database::Create(path, 1);
        database->GetCatalog().CreateTable("gone", Schema({}));
        database->GetCatalog().CreateTable("keep", Schema({}));
        database->GetCatalog().DropTable(0);
        database->Close();
    }
    const auto original = ReadFile(meta);
    Check(original.size() >= 44 && static_cast<unsigned char>(original[8]) == 6,
          "Free-page bootstrap metadata is not v6");
    auto corrupt = [&](const std::string& bytes) {
        WriteFile(meta, bytes);
        Reject<std::exception>([&] { Database::Open(path, 1); });
    };
    auto bytes = original;
    Put(bytes, 36, std::filesystem::file_size(path) / PAGE_SIZE, 8);
    corrupt(bytes);
    bytes = original;
    Put(bytes, 28, 2, 8);
    bytes.insert(44, 8, '\0');
    corrupt(bytes);
    WriteFile(meta, original);
    auto database = Database::Open(path, 1);
    Check(database->GetCatalog().GetTable("keep").GetTableId() == 1, "Valid free metadata did not recover");
    database->Close();

    const auto legacy_path = directory / "legacy.udb";
    const auto legacy_meta = directory / "legacy.meta";
    database = Database::Create(legacy_path, 1);
    database->Close();
    bytes = ReadFile(legacy_meta);
    bytes.resize(24);
    Put(bytes, 8, 1, 4);
    WriteFile(legacy_meta, bytes);
    database = Database::Open(legacy_path, 1);
    Check(database->GetCatalog().ListTables().empty(), "Safe v1 metadata compatibility failed");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-free-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestDiskManager(directory / "disk.udb");
        TestBufferPool(directory / "buffer.udb");
        TestDropAndReuse(directory / "drop.udb");
        TestPersistence(directory);
        TestMetadataValidation(directory);
        std::cout << "Free page tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
