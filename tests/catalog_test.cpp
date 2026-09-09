#include "udb/catalog.h"
#include "udb/tuple.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Exception, typename Function>
void ExpectThrow(Function function) {
    try { function(); } catch (const Exception&) { return; }
    throw std::runtime_error("Expected exception was not thrown");
}

using udb::Column;
using udb::Schema;
using udb::TypeId;
using udb::Tuple;
using udb::Value;

void TestMetadata() {
    const Schema schema({Column("id", TypeId::INTEGER), Column("name", TypeId::VARCHAR, 40)});
    const udb::TableMetadata metadata(12, "users", schema, 7);
    Check(metadata.GetTableId() == 12 && metadata.GetTableName() == "users" &&
          metadata.GetFirstPageId() == 7, "Metadata identity incorrect");
    Check(metadata.GetSchema().GetColumnCount() == 2 &&
          metadata.GetSchema().GetColumn(1).GetMaxLength() == 40, "Metadata schema incorrect");
    ExpectThrow<std::invalid_argument>([&] { udb::TableMetadata invalid(0, "", schema, 0); });
    ExpectThrow<std::invalid_argument>([&] { udb::TableMetadata invalid(0, "x", schema, -1); });
}

void TestCatalog(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    udb::BufferPoolManager pool(disk, 1);
    udb::Catalog catalog(pool);
    Check(catalog.ListTables().empty(), "New catalog must be empty");
    Schema schema({Column("id", TypeId::INTEGER), Column("text", TypeId::VARCHAR, 2000)});
    const auto& first = catalog.CreateTable("users", schema);
    const auto id = first.GetTableId();
    const auto page_id = first.GetFirstPageId();
    Check(id == 0 && page_id == 0, "First table identity incorrect or metadata page allocated");
    Check(&catalog.GetTable(id) == &first && &catalog.GetTable("users") == &first, "Lookup must return owned metadata");
    auto* heap = &catalog.GetTableHeap(id);
    Check(heap == &catalog.GetTableHeap("users") && heap->GetFirstPageId() == page_id,
          "Heap identity differs from metadata");
    Check(!heap->GetFirstRID(), "New table must have no records");
    // Catalog owns a schema copy independent of the caller's later assignment.
    schema = Schema({Column("other", TypeId::BOOLEAN)});
    Check(first.GetSchema().GetColumn(0).GetType() == TypeId::INTEGER &&
          first.GetSchema().GetColumn(1).GetName() == "text", "Catalog did not own schema");
    const auto file_size = std::filesystem::file_size(path);
    ExpectThrow<std::invalid_argument>([&] { catalog.CreateTable("", schema); });
    ExpectThrow<std::invalid_argument>([&] { catalog.CreateTable("users", schema); });
    Check(std::filesystem::file_size(path) == file_size, "Rejected name allocated a page");
    pool.FetchPage(page_id);
    ExpectThrow<std::runtime_error>([&] { catalog.CreateTable("blocked", schema); });
    Check(catalog.ListTables() == std::vector<udb::table_id_t>{id}, "Failed creation published metadata");
    ExpectThrow<std::out_of_range>([&] { catalog.GetTable("blocked"); });
    ExpectThrow<std::out_of_range>([&] { catalog.GetTable(1); });
    Check(std::filesystem::file_size(path) == file_size, "All-pinned failure grew file");
    pool.UnpinPage(page_id, false);
    ExpectThrow<std::logic_error>([&] { pool.UnpinPage(page_id, false); });
    const auto& second = catalog.CreateTable("blocked", schema);
    Check(second.GetTableId() == 1, "Failed create consumed a table ID");
    Check(second.GetFirstPageId() != page_id && second.GetSchema().GetColumn(0).GetType() == TypeId::BOOLEAN,
          "Different tables must have distinct pages and schemas");
    const auto& empty = catalog.CreateTable("empty", Schema({}));
    Check(empty.GetTableId() == 2 && empty.GetSchema().GetColumnCount() == 0, "Valid empty schema rejected");
    Check(catalog.ListTables() == std::vector<udb::table_id_t>({0, 1, 2}), "ListTables order/content wrong");
    const udb::Catalog& readonly = catalog;
    Check(&readonly.GetTableHeap(id) == heap && &readonly.GetTableHeap("users") == heap,
          "Const heap lookup identity wrong");
    ExpectThrow<std::out_of_range>([&] { catalog.GetTable(999); });
    ExpectThrow<std::out_of_range>([&] { catalog.GetTable("missing"); });
    ExpectThrow<std::out_of_range>([&] { catalog.GetTableHeap(999); });
    ExpectThrow<std::out_of_range>([&] { catalog.GetTableHeap("missing"); });
    ExpectThrow<std::out_of_range>([&] { readonly.GetTableHeap(999); });
    ExpectThrow<std::out_of_range>([&] { readonly.GetTableHeap("missing"); });

    std::vector<udb::RID> rows;
    for (int i = 0; i < 8; ++i) {
        const Tuple tuple(first.GetSchema(), {Value::Integer(i), Value::Varchar(std::string(1500, static_cast<char>('a' + i)))});
        rows.push_back(catalog.GetTableHeap("users").InsertRecord(tuple.Serialize(first.GetSchema())));
    }
    const Tuple flag(second.GetSchema(), {Value::Boolean(true)});
    const auto flag_rid = catalog.GetTableHeap(second.GetTableId()).InsertRecord(flag.Serialize(second.GetSchema()));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto tuple = Tuple::Deserialize(catalog.GetTableHeap(id).GetRecord(rows[i]), catalog.GetTable(id).GetSchema());
        Check(tuple.GetValue(0).GetInteger() == static_cast<std::int32_t>(i) &&
              tuple.GetValue(1).GetVarchar() == std::string(1500, static_cast<char>('a' + i)), "Tuple data mismatch");
    }
    Check(Tuple::Deserialize(catalog.GetTableHeap("blocked").GetRecord(flag_rid), second.GetSchema()).GetValue(0).GetBoolean(),
          "Second table data corrupted");
    ExpectThrow<std::out_of_range>([&] { catalog.GetTableHeap(id).GetRecord(flag_rid); });
    ExpectThrow<std::out_of_range>([&] { catalog.GetTableHeap("blocked").GetRecord(rows.front()); });
    catalog.GetTableHeap(id).DeleteRecord(rows.front());
    Check(catalog.GetTableHeap("blocked").GetFirstRID() == flag_rid, "Deleting from one table changed another");
    for (int i = 0; i < 20; ++i) {
        Check(catalog.CreateTable("extra" + std::to_string(i), schema).GetTableId() == static_cast<udb::table_id_t>(i + 3),
              "Table IDs not monotonic");
    }
    Check(&catalog.GetTable(id) == &first && &catalog.GetTableHeap(id) == heap, "Growth invalidated owned references");
    Check(catalog.ListTables().size() == 23, "Catalog growth lost entries");
    // Capacity one plus successful eviction of each accessed table checks pin release.
    for (const auto table_id : catalog.ListTables()) {
        const auto first_page = catalog.GetTable(table_id).GetFirstPageId();
        Check(first_page == catalog.GetTableHeap(table_id).GetFirstPageId(), "Metadata/heap mismatch");
        pool.FetchPage(first_page);
        pool.UnpinPage(first_page, false);
        ExpectThrow<std::logic_error>([&] { pool.UnpinPage(first_page, false); });
    }
    pool.FlushAllPages();
    udb::Catalog fresh(pool);
    Check(fresh.ListTables().empty(), "Memory catalog unexpectedly loaded persistent metadata");
}

void TestNoImplicitFlush(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    udb::BufferPoolManager pool(disk, 1);
    {
        udb::Catalog catalog(pool);
        catalog.CreateTable("memory_only", Schema({}));
    }
    Check(disk.ReadPage(0).data == udb::Page{}.data, "Catalog destructor unexpectedly flushed");
    pool.FetchPage(0);
    pool.UnpinPage(0, false);
    ExpectThrow<std::logic_error>([&] { pool.UnpinPage(0, false); });
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-catalog-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestMetadata();
        TestCatalog(directory / "catalog.udb");
        TestNoImplicitFlush(directory / "memory.udb");
        std::cout << "Catalog tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
