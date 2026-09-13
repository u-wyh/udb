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

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected error");
}

void TestCheckpointAndTailRecovery(const std::filesystem::path& path) {
    auto wal_path = path;
    wal_path.replace_extension(".wal");
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, value VARCHAR(100))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'checkpointed')");
        Check(!database->GetLogManager().GetRecords().empty(),
              "Committed work did not produce a WAL checkpoint prefix");

        database->Checkpoint();
        Check(database->GetLogManager().GetRecords().empty() &&
              std::filesystem::file_size(wal_path) == 0,
              "Checkpoint did not recycle the durable WAL prefix");
        const auto checkpointed = sql.ExecuteSQL("SELECT * FROM t").rows;
        Check(checkpointed.size() == 1 &&
              checkpointed[0].GetValue(1) == Value::Varchar("checkpointed"),
              "Checkpoint changed visible data");

        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'tail')");
        Check(!database->GetLogManager().GetRecords().empty(),
              "Post-checkpoint work did not start a new WAL tail");
        // Simulated crash: leave the committed tail dirty and skip Close().
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        const auto rows = sql.ExecuteSQL("SELECT * FROM t ORDER BY id").rows;
        Check(rows.size() == 2 && rows[0].GetValue(0) == Value::Integer(1) &&
              rows[1].GetValue(0) == Value::Integer(2),
              "Recovery after checkpoint did not replay only the WAL tail");
        database->Close();
    }
}

void TestActiveTransactionBarrier(const std::filesystem::path& path) {
    auto database = Database::Create(path, 2);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER)");
    database->Checkpoint();

    sql.ExecuteSQL("BEGIN");
    sql.ExecuteSQL("INSERT INTO t VALUES (7)");
    Reject([&] { database->Checkpoint(); });
    Check(database->GetLogManager().HasActiveTransactions() &&
          !database->GetLogManager().GetRecords().empty(),
          "Rejected checkpoint discarded an active transaction's WAL");

    sql.ExecuteSQL("ROLLBACK");
    database->Checkpoint();
    Check(database->GetLogManager().GetRecords().empty() &&
          sql.ExecuteSQL("SELECT * FROM t").rows.empty(),
          "Checkpoint after rollback retained transaction state");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-checkpoint-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestCheckpointAndTailRecovery(directory / "tail.udb");
        TestActiveTransactionBarrier(directory / "active.udb");
        std::cout << "Checkpoint tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
