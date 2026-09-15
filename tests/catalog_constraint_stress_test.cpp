#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

void ValidateIndexes(Catalog& catalog) {
    for (const auto index : catalog.ListIndexes()) { catalog.GetIndex(index).GetTree().Validate(); }
}

[[noreturn]] void CrashCatalogChanges(const std::filesystem::path& path) {
    try {
        auto database = Database::Open(path, 4);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE crash_committed (id INTEGER PRIMARY KEY, v INTEGER CHECK (v > 0))");
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("DROP TABLE crash_victim");
        sql.ExecuteSQL("CREATE TABLE crash_loser (id INTEGER UNIQUE)");
        database->GetLogManager().Flush();
        database->GetCatalog().GetBufferPoolManager().FlushAllPages();
        ::_exit(0);
    } catch (...) {
        ::_exit(1);
    }
}

void RunCrash(const std::filesystem::path& path) {
    const auto child = ::fork();
    if (child < 0) { throw std::runtime_error("fork failed"); }
    if (child == 0) { CrashCatalogChanges(path); }
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "Catalog crash fixture failed");
}

void CreateBase(const std::filesystem::path& path) {
    auto database = Database::Create(path, 5);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("CREATE TABLE crash_victim (id INTEGER PRIMARY KEY)");
    sql.ExecuteSQL("CREATE TABLE accounts (id INTEGER PRIMARY KEY, email VARCHAR(20) UNIQUE, "
                   "balance INTEGER NOT NULL DEFAULT 0 CHECK (balance >= 0))");
    sql.ExecuteSQL("CREATE TABLE orders (id INTEGER PRIMARY KEY, account_id INTEGER, "
                   "note VARCHAR(20) DEFAULT 'new', qty INTEGER CHECK (qty > 0), "
                   "FOREIGN KEY (account_id) REFERENCES accounts(id) "
                   "ON DELETE CASCADE ON UPDATE CASCADE)");
    sql.ExecuteSQL("CREATE TABLE audit (id INTEGER PRIMARY KEY, account_id INTEGER, "
                   "FOREIGN KEY (account_id) REFERENCES accounts(id) "
                   "ON DELETE SET NULL ON UPDATE SET NULL)");
    database->Close();
}

void VerifyRecovery(const std::filesystem::path& path) {
    auto database = Database::Open(path, 3);
    auto& catalog = database->GetCatalog();
    Check(catalog.GetTable("crash_committed").GetSchema().GetCheckExpressions().size() == 1,
          "Committed catalog mutation was not recovered");
    Check(catalog.GetTable("crash_victim").GetSchema().GetColumn(0).IsPrimaryKey(),
          "Loser DROP TABLE was not undone");
    Reject([&] { static_cast<void>(catalog.GetTable("crash_loser")); });
    ValidateIndexes(catalog);
    database->Close();
}

void ExerciseConstraints(const std::filesystem::path& path) {
    auto database = Database::Open(path, 6);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);

    sql.ExecuteSQL("BEGIN");
    sql.ExecuteSQL("CREATE TABLE rolled_back (id INTEGER PRIMARY KEY)");
    sql.ExecuteSQL("ROLLBACK");
    Reject([&] { static_cast<void>(catalog.GetTable("rolled_back")); });

    sql.ExecuteSQL("INSERT INTO accounts VALUES (1, 'a@u', DEFAULT)");
    sql.ExecuteSQL("INSERT INTO orders VALUES (10, 1, DEFAULT, 2)");
    sql.ExecuteSQL("INSERT INTO audit VALUES (20, 1)");
    Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (1, 'other', 1)"); });
    Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (2, 'a@u', 1)"); });
    Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (2, 'b@u', -1)"); });
    Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (2, 'b@u', NULL)"); });
    Reject([&] { sql.ExecuteSQL("INSERT INTO orders VALUES (11, 999, 'bad', 1)"); });

    SqlEngine snapshot(catalog, IsolationLevel::SnapshotIsolation);
    SqlEngine writer(catalog);
    snapshot.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    Check(snapshot.ExecuteSQL("SELECT balance FROM accounts WHERE id = 1").rows[0]
                  .GetValue(0).GetInteger() == 0,
          "Snapshot fixture is wrong");
    writer.ExecuteSQL("UPDATE accounts SET balance = 5 WHERE id = 1");
    Check(snapshot.ExecuteSQL("SELECT balance FROM accounts WHERE id = 1").rows[0]
                  .GetValue(0).GetInteger() == 0,
          "Snapshot observed a later committed value");
    snapshot.ExecuteSQL("COMMIT");

    sql.ExecuteSQL("BEGIN ISOLATION LEVEL SERIALIZABLE");
    sql.ExecuteSQL("INSERT INTO accounts VALUES (2, 'b@u', 7)");
    sql.ExecuteSQL("INSERT INTO orders VALUES (11, 2, 'serial', 3)");
    sql.ExecuteSQL("COMMIT");

    sql.ExecuteSQL("UPDATE accounts SET id = 3 WHERE id = 1");
    Check(sql.ExecuteSQL("SELECT * FROM orders WHERE account_id = 3").rows.size() == 1,
          "Cascade update failed during stress");
    Check(sql.ExecuteSQL("SELECT account_id FROM audit WHERE id = 20").rows[0]
              .GetValue(0).IsNull(), "SET NULL failed during stress");
    sql.ExecuteSQL("DELETE FROM accounts WHERE id = 3");
    Check(sql.ExecuteSQL("SELECT * FROM orders WHERE id = 10").rows.empty(),
          "Cascade delete failed during stress");

    sql.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    sql.ExecuteSQL("INSERT INTO accounts VALUES (50, 'checkpoint', 1)");
    database->Checkpoint();
    sql.ExecuteSQL("ROLLBACK");
    Check(sql.ExecuteSQL("SELECT * FROM accounts WHERE id = 50").rows.empty(),
          "Fuzzy checkpoint lost transaction undo state");
    ValidateIndexes(catalog);
    database->Checkpoint();
    database->Close();
}

void CheckSchemaLock(const std::filesystem::path& path) {
    auto database = Database::Open(path, 4);
    SqlEngine ddl(database->GetCatalog());
    SqlEngine reader(database->GetCatalog());
    ddl.ExecuteSQL("BEGIN");
    ddl.ExecuteSQL("CREATE TABLE pending_stress (id INTEGER PRIMARY KEY)");
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        started = true;
        Check(reader.ExecuteSQL("SELECT * FROM accounts").rows.size() == 1,
              "Concurrent schema reader returned wrong data");
        finished = true;
    });
    while (!started.load()) { std::this_thread::yield(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(!finished.load(), "DDL schema lock did not block the concurrent query");
    ddl.ExecuteSQL("COMMIT");
    worker.join();
    Check(finished.load(), "Concurrent query did not resume after DDL commit");
    database->Close();
}

void ReopenRepeatedly(const std::filesystem::path& path) {
    for (int reopen = 0; reopen < 3; ++reopen) {
        auto database = Database::Open(path, reopen == 0 ? 3 : 1);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        Check(sql.ExecuteSQL("SELECT email FROM accounts WHERE id = 2").rows.size() == 1,
              "Repeated recovery lost constrained table data");
        Check(sql.ExecuteSQL("SELECT * FROM orders WHERE account_id = 2").rows.size() == 1,
              "Repeated recovery lost child data");
        ValidateIndexes(catalog);
        database->Close();
    }
}

void RunRound(const std::filesystem::path& path) {
    CreateBase(path);
    RunCrash(path);
    VerifyRecovery(path);
    ExerciseConstraints(path);
    CheckSchemaLock(path);
    ReopenRepeatedly(path);
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-catalog-constraint-stress-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        RunRound(directory / "round-a.udb");
        RunRound(directory / "round-b.udb");
        std::cout << "Catalog and constraint stress tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
