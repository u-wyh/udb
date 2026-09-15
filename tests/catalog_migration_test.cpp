#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void Append(std::string& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

void AppendString(std::string& bytes, const std::string& value) {
    Append(bytes, value.size(), 4);
    bytes += value;
}

std::uint64_t Read(const std::string& bytes, std::size_t offset, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
    }
    return value;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(static_cast<bool>(output), "Cannot write legacy metadata fixture");
}

std::string LegacyMetadata(std::uint32_t version, page_id_t first_page,
                           std::optional<page_id_t> index_header, timestamp_t commit_timestamp,
                           const std::vector<page_id_t>& free_pages = {},
                           table_id_t table_id = 0, table_id_t next_table_id = 1) {
    std::string bytes;
    bytes += "UDBMETA1";
    Append(bytes, version, 4);
    Append(bytes, 1, 4);
    Append(bytes, next_table_id, 8);
    if (version >= 5) { Append(bytes, commit_timestamp, 8); }
    if (version >= 2) {
        Append(bytes, free_pages.size(), 8);
        for (const auto page_id : free_pages) { Append(bytes, static_cast<std::uint64_t>(page_id), 8); }
    }
    Append(bytes, table_id, 8);
    AppendString(bytes, "t");
    Append(bytes, static_cast<std::uint64_t>(first_page), 8);
    Append(bytes, 1, 4);
    AppendString(bytes, "id");
    Append(bytes, 1, 1);  // INTEGER
    Append(bytes, 0, 4);
    if (version >= 3) {
        Append(bytes, index_header ? 1 : 0, 8);
        Append(bytes, index_header ? 1 : 0, 8);
        if (index_header) {
            Append(bytes, 0, 8);
            AppendString(bytes, "idx");
            Append(bytes, table_id, 8);
            if (version >= 4) { Append(bytes, 1, 8); }
            Append(bytes, 0, 8);
            Append(bytes, static_cast<std::uint64_t>(*index_header), 8);
        }
    }
    return bytes;
}

void TestVersions(const std::filesystem::path& directory) {
    for (std::uint32_t version = 1; version <= 5; ++version) {
        const auto path = directory / ("v" + std::to_string(version) + ".udb");
        auto metadata = path;
        metadata.replace_extension(".meta");
        auto checkpoint = path;
        checkpoint.replace_extension(".wal.ckpt");
        page_id_t first_page = -1;
        page_id_t index_header = -1;
        timestamp_t commit_timestamp = 0;
        {
            auto database = Database::Create(path, 2);
            SqlEngine engine(database->GetCatalog());
            engine.ExecuteSQL("CREATE TABLE t (id INTEGER)");
            engine.ExecuteSQL("INSERT INTO t VALUES (17)");
            engine.ExecuteSQL("CREATE INDEX idx ON t(id)");
            first_page = database->GetCatalog().GetTable("t").GetFirstPageId();
            index_header = database->GetCatalog().GetIndex("idx").GetMetadata().GetHeaderPageId();
            commit_timestamp = TransactionManager::GetLastCommitTimestamp();
            database->Close();
        }
        const auto checkpoint_before = ReadFile(checkpoint);
        WriteFile(metadata, LegacyMetadata(version, first_page,
                                           version >= 3 ? std::optional<page_id_t>(index_header) : std::nullopt,
                                           commit_timestamp));
        {
            auto database = Database::Open(path, 1);
            Check(ReadFile(checkpoint) == checkpoint_before,
                  "Catalog migration changed ARIES checkpoint state");
            SqlEngine engine(database->GetCatalog());
            const auto result = engine.ExecuteSQL("SELECT * FROM t");
            Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Integer(17),
                  "Migrated legacy table data is wrong");
            if (version >= 3) {
                const auto& index = database->GetCatalog().GetIndex("idx");
                Check(index.GetMetadata().GetHeaderPageId() == index_header &&
                      index.GetTree().GetValue(17).has_value(), "Migrated legacy index root is wrong");
            } else {
                Check(database->GetCatalog().ListIndexes().empty(), "Legacy pre-index catalog gained an index");
            }
            Check(database->GetCatalog().CreateTable("next", Schema({})).GetTableId() == 1,
                  "Migrated next table ID regressed");
            if (version == 5) {
                Check(TransactionManager::GetLastCommitTimestamp() >= commit_timestamp,
                      "Migrated commit timestamp regressed");
            }
            const auto migrated = ReadFile(metadata);
            Check(migrated.size() == 36 && static_cast<unsigned char>(migrated[8]) == 6,
                  "Legacy metadata was not migrated during open");
            Check(Read(migrated, 12, 8) < std::filesystem::file_size(path) / PAGE_SIZE,
                  "Migrated catalog root is invalid");
            database->Close();
        }
    }
}

void TestFreePageMigration(const std::filesystem::path& directory) {
    const auto path = directory / "free.udb";
    auto metadata = path;
    metadata.replace_extension(".meta");
    page_id_t freed_page = -1;
    page_id_t kept_page = -1;
    timestamp_t timestamp = 0;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        freed_page = catalog.CreateTable("victim", Schema({})).GetFirstPageId();
        kept_page = catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER)})).GetFirstPageId();
        catalog.DropTable(0);
        timestamp = TransactionManager::GetLastCommitTimestamp();
        database->Close();
    }
    WriteFile(metadata, LegacyMetadata(5, kept_page, std::nullopt, timestamp,
                                       {freed_page}, 1, 2));
    {
        auto database = Database::Open(path, 1);
        const auto migrated = ReadFile(metadata);
        Check(Read(migrated, 12, 8) == static_cast<std::uint64_t>(freed_page),
              "Migration did not reuse the legacy free page for system catalog storage");
        Check(Read(migrated, 28, 8) == 0, "Migrated free-page state still includes catalog root");
        Check(database->GetCatalog().GetTable("t").GetFirstPageId() == kept_page,
              "Migration changed the surviving table root");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-catalog-migration-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestVersions(directory);
        TestFreePageMigration(directory);
        std::cout << "Catalog migration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
