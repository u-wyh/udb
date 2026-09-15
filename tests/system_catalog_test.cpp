#include "udb/database.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Run(const std::filesystem::path& directory) {
    const auto path = directory / "catalog.udb";
    const auto metadata = directory / "catalog.meta";
    {
        auto database = udb::Database::Create(path, 1);
        const udb::Schema schema({udb::Column("id", udb::TypeId::BIGINT),
                                  udb::Column("payload", udb::TypeId::VARCHAR, 512)});
        for (std::size_t i = 0; i < 80; ++i) {
            const auto name = "table_" + std::to_string(i) + std::string(80, 'x');
            database->GetCatalog().CreateTable(name, schema);
        }
        database->GetCatalog().CreateIndex("catalog_index", 0, 0);
        database->Close();
    }
    const auto bootstrap = ReadFile(metadata);
    Check(bootstrap.size() == 36 && bootstrap.substr(0, 8) == "UDBMETA1" && bootstrap[8] == 6,
          "Catalog bootstrap is not minimal v6 metadata");
    Check(std::filesystem::file_size(path) > 82 * udb::PAGE_SIZE,
          "Large system catalog did not span catalog pages");
    {
        auto database = udb::Database::Open(path, 1);
        const auto ids = database->GetCatalog().ListTables();
        Check(ids.size() == 80 && ids.front() == 0 && ids.back() == 79,
              "System catalog tables were not restored");
        Check(database->GetCatalog().GetTable(37).GetTableName().find("table_37") == 0,
              "System catalog table name was not restored");
        Check(database->GetCatalog().GetTable(37).GetSchema().GetColumn(1).GetMaxLength() == 512,
              "System catalog column metadata was not restored");
        const auto indexes = database->GetCatalog().ListIndexes();
        Check(indexes.size() == 1 && database->GetCatalog().GetIndex(indexes[0]).GetMetadata().GetTableId() == 0,
              "System catalog index metadata was not restored");
        database->Close();
    }
    Check(ReadFile(metadata) == bootstrap, "Catalog contents leaked back into bootstrap metadata");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-system-catalog-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        Run(directory);
        std::cout << "System catalog tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
