#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/system_catalog_storage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected operation to fail");
}

void CheckDefaults(const Schema& schema) {
    Check(schema.GetColumnCount() == 5 && schema.GetColumn(0).IsNotNull() &&
              !schema.GetColumn(0).GetDefaultValue(),
          "NOT NULL metadata is wrong");
    const auto& label = schema.GetColumn(1);
    Check(label.IsNotNull() && label.GetDefaultValue() &&
              label.GetDefaultValue()->GetVarchar() == "new",
          "VARCHAR DEFAULT metadata is wrong");
    Check(schema.GetColumn(2).GetDefaultValue()->GetBoolean(),
          "BOOLEAN DEFAULT metadata is wrong");
    Check(schema.GetColumn(3).GetDefaultValue()->GetBigInt() == 2147483648LL,
          "BIGINT DEFAULT metadata is wrong");
    Check(schema.GetColumn(4).GetDefaultValue()->IsNull(),
          "NULL DEFAULT metadata is wrong");
}


void Put(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

void PutString(std::vector<unsigned char>& bytes, const std::string& value) {
    Put(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::uint64_t ReadAt(const std::filesystem::path& path, std::size_t offset, std::size_t width) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const auto byte = input.get();
        if (byte < 0) { throw std::runtime_error("Cannot read metadata fixture"); }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << (8 * i);
    }
    return value;
}

void TestVersionOneCatalogCompatibility(const std::filesystem::path& path) {
    page_id_t first_page = -1;
    {
        auto database = Database::Create(path, 2);
        first_page = database->GetCatalog().CreateTable(
            "legacy", Schema({Column("id", TypeId::INTEGER)})).GetFirstPageId();
        database->Close();
    }
    auto metadata = path;
    metadata.replace_extension(".meta");
    const auto root = static_cast<page_id_t>(ReadAt(metadata, 12, 8));
    std::vector<unsigned char> bytes;
    Put(bytes, 0x3154414353595355ULL, 8);
    Put(bytes, 1, 4);
    Put(bytes, 1, 8);
    Put(bytes, 1, 8);
    Put(bytes, 0, 8);
    PutString(bytes, "legacy");
    Put(bytes, static_cast<std::uint64_t>(first_page), 8);
    Put(bytes, 1, 4);
    PutString(bytes, "id");
    Put(bytes, 1, 1);
    Put(bytes, 0, 4);
    Put(bytes, 0, 8);
    Put(bytes, 0, 8);
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 2);
        SystemCatalogStorage(pool, root).Write(bytes);
        pool.FlushAllPages();
        disk.Sync();
    }
    {
        auto database = Database::Open(path, 2);
        const auto& column = database->GetCatalog().GetTable("legacy").GetSchema().GetColumn(0);
        Check(!column.IsNotNull() && !column.GetDefaultValue(),
              "Version 1 system catalog did not default new constraints to nullable");
        database->Close();
    }
}

void TestConstraints(const std::filesystem::path& path) {
    const auto parsed = std::get<CreateTableStatement>(Parser::Parse(
        "CREATE TABLE syntax (id INTEGER NOT NULL, value VARCHAR(4) DEFAULT 'x')"));
    Check(parsed.columns[0].not_null && parsed.columns[1].default_value.has_value(),
          "Column constraints were not parsed");
    Reject([&] { Parser::Parse("CREATE TABLE bad (id INTEGER NOT)"); });
    Reject([&] { Parser::Parse("CREATE TABLE bad (id INTEGER DEFAULT)"); });

    {
        auto database = Database::Create(path, 3);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL(
            "CREATE TABLE settings (id INTEGER NOT NULL, "
            "label VARCHAR(10) DEFAULT 'new' NOT NULL, "
            "active BOOLEAN DEFAULT TRUE, score BIGINT DEFAULT 2147483648, "
            "note VARCHAR(10) DEFAULT NULL)");
        CheckDefaults(database->GetCatalog().GetTable("settings").GetSchema());

        sql.ExecuteSQL("INSERT INTO settings VALUES (1, DEFAULT, DEFAULT, DEFAULT, DEFAULT)");
        auto result = sql.ExecuteSQL("SELECT * FROM settings");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0).GetInteger() == 1 &&
                  result.rows[0].GetValue(1).GetVarchar() == "new" &&
                  result.rows[0].GetValue(2).GetBoolean() &&
                  result.rows[0].GetValue(3).GetBigInt() == 2147483648LL &&
                  result.rows[0].GetValue(4).IsNull(),
              "INSERT DEFAULT did not materialize column defaults");

        Reject([&] { sql.ExecuteSQL("INSERT INTO settings VALUES (NULL, 'x', TRUE, 2147483648, NULL)"); });
        Reject([&] { sql.ExecuteSQL("INSERT INTO settings VALUES (DEFAULT, 'x', TRUE, 2147483648, NULL)"); });
        Reject([&] { sql.ExecuteSQL("UPDATE settings SET id = NULL WHERE id = 1"); });
        Reject([&] { sql.ExecuteSQL("UPDATE settings SET id = DEFAULT WHERE id = 1"); });
        sql.ExecuteSQL("UPDATE settings SET label = 'changed', active = FALSE WHERE id = 1");
        sql.ExecuteSQL("UPDATE settings SET label = DEFAULT, active = DEFAULT, note = DEFAULT WHERE id = 1");
        result = sql.ExecuteSQL("SELECT label, active, note FROM settings WHERE id = 1");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0).GetVarchar() == "new" &&
                  result.rows[0].GetValue(1).GetBoolean() && result.rows[0].GetValue(2).IsNull(),
              "UPDATE DEFAULT did not restore column defaults");

        Reject([&] { sql.ExecuteSQL("CREATE TABLE bad_default (v VARCHAR(2) DEFAULT 'long')"); });
        Reject([&] { sql.ExecuteSQL("CREATE TABLE bad_null (v INTEGER NOT NULL DEFAULT NULL)"); });
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        CheckDefaults(database->GetCatalog().GetTable("settings").GetSchema());
        Reject([&] { sql.ExecuteSQL("UPDATE settings SET label = NULL WHERE id = 1"); });
        const auto result = sql.ExecuteSQL("SELECT label, score FROM settings WHERE id = 1");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0).GetVarchar() == "new" &&
                  result.rows[0].GetValue(1).GetBigInt() == 2147483648LL,
              "Constraint metadata or data changed after reopen");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-column-constraint-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestConstraints(directory / "constraints.udb");
        TestVersionOneCatalogCompatibility(directory / "legacy.udb");
        std::cout << "Column constraint tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
