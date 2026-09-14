#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestReadPathsAndWriteSkew(const std::filesystem::path& path) {
    auto database = Database::Create(path, 3);
    auto& catalog = database->GetCatalog();
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE accounts (id INTEGER, balance INTEGER)");
    setup.ExecuteSQL("CREATE TABLE tags (id INTEGER)");
    setup.ExecuteSQL("INSERT INTO accounts VALUES (1, 100)");
    setup.ExecuteSQL("INSERT INTO accounts VALUES (2, 100)");
    setup.ExecuteSQL("INSERT INTO tags VALUES (1)");
    setup.ExecuteSQL("CREATE INDEX idx_accounts_id ON accounts(id)");

    SqlEngine paths(catalog);
    paths.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
    Check(paths.ExecuteSQL("SELECT id FROM accounts WHERE id = 1").rows.size() == 1,
          "Serializable point scan failed");
    Check(paths.ExecuteSQL("SELECT id FROM accounts WHERE id >= 1").rows.size() == 2,
          "Serializable range scan failed");
    Check(paths.ExecuteSQL("SELECT * FROM accounts WHERE balance > 0").rows.size() == 2,
          "Serializable sequential scan failed");
    Check(paths.ExecuteSQL("SELECT COUNT(*) FROM accounts").rows.size() == 1,
          "Serializable aggregate scan failed");
    Check(paths.ExecuteSQL("SELECT accounts.id FROM accounts CROSS JOIN tags").rows.size() == 2,
          "Serializable join scan failed");
    Check(catalog.GetTransactionManager().GetPredicateSireadCount() >= 4 &&
              catalog.GetTransactionManager().GetTupleSireadCount() >= 2,
          "SQL scans did not register predicate and tuple SIREADs");
    paths.ExecuteSQL("ROLLBACK");

    SqlEngine first(catalog);
    SqlEngine second(catalog);
    first.ExecuteSQL("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    second.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
    Check(first.ExecuteSQL("SELECT balance FROM accounts WHERE id = 1").rows.size() == 1 &&
              second.ExecuteSQL("SELECT * FROM accounts WHERE balance >= 0").rows.size() == 2,
          "Serializable write-skew fixture reads failed");
    first.ExecuteSQL("UPDATE accounts SET balance = 0 WHERE id = 1");
    first.ExecuteSQL("COMMIT");
    second.ExecuteSQL("UPDATE accounts SET balance = 0 WHERE id = 2");
    bool rejected = false;
    try {
        second.ExecuteSQL("COMMIT");
    } catch (const SerializationFailure&) {
        rejected = true;
    }
    Check(rejected && !second.HasActiveTransaction(),
          "SSI did not abort a dangerous SQL transaction");

    const auto rows = setup.ExecuteSQL("SELECT id, balance FROM accounts ORDER BY id").rows;
    Check(rows.size() == 2 && rows[0].GetValue(1) == Value::Integer(0) &&
              rows[1].GetValue(1) == Value::Integer(100),
          "Serialization failure did not preserve committed SQL state");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-serializable-sql-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestReadPathsAndWriteSkew(directory / "ssi.udb");
        std::cout << "Serializable SQL integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
