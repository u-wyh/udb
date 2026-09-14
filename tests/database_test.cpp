#include "udb/database.h"
#include "udb/tuple.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(static_cast<bool>(input), "Cannot read test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    Check(static_cast<bool>(output), "Cannot write test file");
}

void Put(std::string& bytes, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) { bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255); }
}

using udb::Database;
using udb::Column;
using udb::Schema;
using udb::TypeId;
using udb::Tuple;
using udb::Value;

void TestLifecycle(const std::filesystem::path& directory) {
    const auto path = directory / "life.udb";
    const auto meta = directory / "life.meta";
    {
        auto database = Database::Create(path, 1);
        Check(std::filesystem::exists(path) && std::filesystem::exists(meta), "Create did not produce two files");
        Check(std::filesystem::file_size(path) == 0, "Create reserved a data page");
        Check(database->GetCatalog().ListTables().empty(), "New catalog not empty");
        const auto bytes = ReadFile(meta);
        Check(bytes.size() == 56 && bytes.substr(0, 8) == "UDBMETA1" && bytes[8] == 5, "Header fixture mismatch");
        database->Close();
        database->Close();
        ExpectThrow<std::logic_error>([&] { database->GetCatalog(); });
        ExpectThrow<std::logic_error>([&] { database->Flush(); });
    }
    const Schema schema({Column("flag", TypeId::BOOLEAN), Column("id", TypeId::INTEGER),
                         Column("big", TypeId::BIGINT), Column("text", TypeId::VARCHAR, 2000)});
    std::vector<udb::RID> rids;
    udb::page_id_t first;
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        Check(catalog.ListTables().empty(), "Empty reopen failed");
        const auto& table = catalog.CreateTable("users", schema);
        first = table.GetFirstPageId();
        Check(first == 0, "First table should use page zero");
        for (int i = 0; i < 10; ++i) {
            const Tuple tuple(schema, {Value::Boolean(i % 2 == 0), Value::Integer(i), Value::Null(TypeId::BIGINT),
                                       Value::Varchar(std::string(1400, static_cast<char>('a' + i)))});
            rids.push_back(catalog.GetTableHeap("users").InsertRecord(tuple.Serialize(schema)));
        }
        Check(rids.back().page_id > first, "Table did not span multiple pages");
        database->Flush();
        Check(!std::filesystem::exists(directory / "life.meta.tmp"), "Successful save left temporary file");
        // Destructor is not needed to persist a successful explicit Flush.
    }
    const auto pages = std::filesystem::file_size(path);
    {
        auto database = Database::Open(path, 1);
        Check(std::filesystem::file_size(path) == pages, "Open allocated new table pages");
        auto& catalog = database->GetCatalog();
        const auto& table = catalog.GetTable("users");
        Check(table.GetTableId() == 0 && &table == &catalog.GetTable(0) && table.GetFirstPageId() == first,
              "Restored table identity mismatch");
        const auto& restored = table.GetSchema();
        Check(restored.GetColumnCount() == schema.GetColumnCount(), "Restored column count mismatch");
        for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) {
            Check(restored.GetColumn(i).GetName() == schema.GetColumn(i).GetName() &&
                  restored.GetColumn(i).GetType() == schema.GetColumn(i).GetType() &&
                  restored.GetColumn(i).GetMaxLength() == schema.GetColumn(i).GetMaxLength(), "Restored schema mismatch");
        }
        auto& heap = catalog.GetTableHeap(0);
        std::size_t index = 0;
        for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
            Check(index < rids.size() && *rid == rids[index], "Reopen scan RID mismatch");
            const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), restored);
            Check(tuple.GetValue(0).GetBoolean() == (index % 2 == 0) &&
                  tuple.GetValue(1).GetInteger() == static_cast<std::int32_t>(index) &&
                  tuple.GetValue(2).IsNull() && tuple.GetValue(3).GetVarchar() == std::string(1400, static_cast<char>('a' + index)),
                  "Restored tuple mismatch");
            ++index;
        }
        Check(index == 10, "Reopen lost tuples");
        heap.DeleteRecord(rids[4]);
        Check(catalog.CreateTable("other", Schema({Column("x", TypeId::BIGINT)})).GetTableId() == 1,
              "Next table ID not restored");
        Check(catalog.CreateTable("empty", Schema({})).GetTableId() == 2, "Multiple table create failed");
        database->Close();
    }
    const auto expanded = std::filesystem::file_size(path);
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        Check(catalog.ListTables() == std::vector<udb::table_id_t>({0, 1, 2}), "Multiple tables not restored");
        Check(catalog.GetTable(1).GetTableName() == "other" && catalog.GetTable(2).GetSchema().GetColumnCount() == 0,
              "Restored extra metadata mismatch");
        Check(std::filesystem::file_size(path) == expanded, "Reopen grew data file");
        auto& heap = catalog.GetTableHeap("users");
        ExpectThrow<std::out_of_range>([&] { heap.GetRecord(rids[4]); });
        std::size_t count = 0;
        for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
            Check(*rid != rids[4], "Deleted record reappeared in scan");
            ++count;
        }
        Check(count == 9, "Deletion persistence scan count wrong");
        Check(catalog.CreateTable("after", Schema({})).GetTableId() == 3, "ID conflict after second restart");
        database->Close();
    }
}

void TestCorruption(const std::filesystem::path& directory) {
    const auto path = directory / "bad.udb";
    const auto meta = directory / "bad.meta";
    {
        auto database = Database::Create(path, 1);
        database->GetCatalog().CreateTable("a", Schema({Column("i", TypeId::INTEGER)}));
        database->GetCatalog().CreateTable("b", Schema({Column("j", TypeId::BOOLEAN)}));
        database->Close();
    }
    const auto original = ReadFile(meta);
    const auto data = ReadFile(path);
    Check(original.size() == 126, "Metadata fixture length unexpected");
    auto reject = [&](const std::string& bytes) {
        WriteFile(meta, bytes);
        ExpectThrow<std::exception>([&] { Database::Open(path, 1); });
        Check(ReadFile(path) == data && ReadFile(meta) == bytes, "Failed Open mutated database");
    };
    for (std::size_t size = 0; size < original.size(); ++size) { reject(original.substr(0, size)); }
    for (const auto offset : {std::size_t{0}, std::size_t{8}, std::size_t{70}}) {
        auto bytes = original;
        bytes[offset] = static_cast<char>(255);  // magic, version, type
        reject(bytes);
    }
    auto bytes = original;
    Put(bytes, 75, 0, 8); reject(bytes);  // Duplicate table ID.
    bytes = original; bytes[87] = 'a'; reject(bytes);  // Duplicate name.
    bytes = original; Put(bytes, 53, 999, 8); reject(bytes);  // Nonexistent first page.
    bytes = original; Put(bytes, 53, UINT64_MAX, 8); reject(bytes);  // Negative ID encoding.
    bytes = original; Put(bytes, 88, 0, 8); reject(bytes);  // Shared first page.
    bytes = original; Put(bytes, 16, 1, 8); reject(bytes);  // next_table_id <= maximum.
    bytes = original; Put(bytes, 12, UINT32_MAX, 4); reject(bytes);
    bytes = original; Put(bytes, 24, UINT64_MAX, 8); reject(bytes);  // Transaction timestamp bit.
    bytes = original; Put(bytes, 32, UINT64_MAX, 8); reject(bytes);
    bytes = original; Put(bytes, 48, UINT32_MAX, 4); reject(bytes);
    bytes = original; Put(bytes, 61, UINT32_MAX, 4); reject(bytes);
    bytes = original; Put(bytes, 71, 1, 4); reject(bytes);  // INTEGER cannot have max length.
    reject(original + "x");
    WriteFile(meta, original);
    auto database = Database::Open(path, 1);
    Check(database->GetCatalog().ListTables().size() == 2, "Valid metadata failed after corruption tests");
    database.reset();
    bytes = original;
    Put(bytes, 16, 42, 8);
    WriteFile(meta, bytes);
    database = Database::Open(path, 1);
    Check(database->GetCatalog().CreateTable("gap", Schema({})).GetTableId() == 42,
          "Persisted next_table_id was ignored");
}

void TestFailures(const std::filesystem::path& directory) {
    const auto path = directory / "failure.udb";
    const auto meta = directory / "failure.meta";
    const auto temporary = directory / "failure.meta.tmp";
    ExpectThrow<std::exception>([&] { Database::Open(path); });
    Check(!std::filesystem::exists(path), "Open created a missing file");
    ExpectThrow<std::invalid_argument>([&] { Database::Create(directory / "wrong.ext"); });
    ExpectThrow<std::invalid_argument>([&] { Database::Create(path, 0); });
    auto database = Database::Create(path, 1);
    const auto original = ReadFile(meta);
    ExpectThrow<std::runtime_error>([&] { Database::Create(path); });
    Check(ReadFile(meta) == original, "Create overwrote existing database");
    database->GetCatalog().CreateTable("retry", Schema({}));
    std::filesystem::create_directory(temporary);
    ExpectThrow<std::runtime_error>([&] { database->Close(); });
    Check(ReadFile(meta) == original && database->GetCatalog().GetTable("retry").GetTableId() == 0,
          "Failed Close destroyed catalog or metadata");
    std::filesystem::remove(temporary);
    // Force failure after writing the temp file, at the rename step.
    const auto backup = directory / "failure.meta.backup";
    std::filesystem::rename(meta, backup);
    std::filesystem::create_directory(meta);
    WriteFile(meta / "keep", "sentinel");
    ExpectThrow<std::filesystem::filesystem_error>([&] { database->Flush(); });
    Check(!std::filesystem::exists(temporary) && ReadFile(backup) == original &&
          ReadFile(meta / "keep") == "sentinel", "Rename failure cleanup damaged existing files");
    std::filesystem::remove(meta / "keep");
    std::filesystem::remove(meta);
    std::filesystem::rename(backup, meta);
    database->Close();
    Check(!std::filesystem::exists(temporary), "Retry left temp metadata");
    {
        auto reopened = Database::Open(path, 1);
        Check(reopened->GetCatalog().GetTable("retry").GetTableId() == 0, "Retry did not persist metadata");
    }
    const auto saved = ReadFile(meta);
    std::filesystem::remove(meta);
    ExpectThrow<std::runtime_error>([&] { Database::Open(path); });
    Check(!std::filesystem::exists(meta), "Open created missing metadata");
    WriteFile(meta, saved);
    std::filesystem::remove(path);
    ExpectThrow<std::runtime_error>([&] { Database::Open(path); });
    Check(!std::filesystem::exists(path), "Open recreated missing data file");
    ExpectThrow<std::runtime_error>([&] { Database::Create(path); });
    Check(ReadFile(meta) == saved, "Create overwrote orphan metadata");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-database-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestLifecycle(directory);
        TestCorruption(directory);
        TestFailures(directory);
        std::cout << "Database tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
