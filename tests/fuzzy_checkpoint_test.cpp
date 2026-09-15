#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void CreateTable(const std::filesystem::path& path) {
    auto database = Database::Create(path, 2);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER, value VARCHAR(100))");
    database->Close();
}

void TestLoserAcrossCheckpoint(const std::filesystem::path& path) {
    CreateTable(path);
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'loser')");
        const auto wal_size = database->GetLogManager().GetRecords().size();
        database->Checkpoint();
        const auto checkpoint = database->GetLogManager().ReadCheckpoint();
        Check(checkpoint && checkpoint->transaction_table.size() == 1 &&
              !checkpoint->dirty_page_table.empty(),
              "Fuzzy checkpoint did not persist TT and DPT state");
        Check(database->GetLogManager().GetRecords().size() == wal_size,
              "Fuzzy checkpoint recycled an active transaction's WAL");
        // Simulated crash while the transaction remains active.
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        Check(sql.ExecuteSQL("SELECT * FROM t").rows.empty(),
              "Recovery retained a loser persisted by fuzzy checkpoint");
        database->Close();
    }
}

void TestCommitAfterCheckpoint(const std::filesystem::path& path) {
    CreateTable(path);
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'winner')");
        database->Checkpoint();
        sql.ExecuteSQL("COMMIT");
        // Simulated crash after commit without another checkpoint.
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        const auto rows = sql.ExecuteSQL("SELECT * FROM t").rows;
        Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(2),
              "Commit after fuzzy checkpoint was not recovered");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-fuzzy-checkpoint-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestLoserAcrossCheckpoint(directory / "loser.udb");
        TestCommitAfterCheckpoint(directory / "winner.udb");
        std::cout << "Fuzzy checkpoint tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
