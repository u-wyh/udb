#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::int32_t Read(SqlEngine& sql, const std::string& query) {
    const auto rows = sql.ExecuteSQL(query).rows;
    Check(rows.size() == 1, "Expected one READ COMMITTED row");
    return rows[0].GetValue(0).GetInteger();
}

void TestReadCommitted(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");

    SqlEngine reader(catalog, IsolationLevel::ReadCommitted);
    const auto id = *reader.ExecuteSQL("BEGIN ISOLATION LEVEL READ COMMITTED").transaction_id;
    auto& transaction = catalog.GetTransactionManager().GetTransaction(id);
    Check(Read(reader, "SELECT value FROM t WHERE id = 1") == 10,
          "Initial index snapshot is wrong");
    const auto first_snapshot = transaction.GetReadTimestamp();

    SqlEngine writer(catalog);
    writer.ExecuteSQL("UPDATE t SET value = 20 WHERE id = 1");
    Check(Read(reader, "SELECT value FROM t WHERE id = 1") == 20 &&
              transaction.GetReadTimestamp() > first_snapshot,
          "READ COMMITTED did not refresh its statement snapshot");
    Check(Read(reader, "SELECT value FROM t WHERE id >= 1 AND id <= 1") == 20 &&
              Read(reader, "SELECT value FROM t WHERE value != 99") == 20,
          "READ COMMITTED differs across index, range, and sequential scans");
    Check(transaction.GetSharedTableLocks().empty(),
          "MVCC READ COMMITTED retained a shared table lock");

    SqlEngine loser(catalog, IsolationLevel::ReadCommitted);
    loser.ExecuteSQL("BEGIN ISOLATION LEVEL READ COMMITTED");
    loser.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 1");
    Check(Read(reader, "SELECT value FROM t WHERE id = 1") == 20,
          "READ COMMITTED exposed an uncommitted version");
    loser.ExecuteSQL("ROLLBACK");
    Check(Read(reader, "SELECT value FROM t WHERE id = 1") == 20,
          "Writer rollback changed the committed snapshot");
    reader.ExecuteSQL("COMMIT");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-rc-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 2);
        TestReadCommitted(database->GetCatalog());
        database->Close();
        std::cout << "MVCC READ COMMITTED tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
