#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

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

void TestTransactionalDdl(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 3);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("CREATE TABLE rolled_back (id INTEGER)");
        sql.ExecuteSQL("CREATE INDEX rolled_back_idx ON rolled_back(id)");
        sql.ExecuteSQL("ROLLBACK");
        Reject([&] { static_cast<void>(catalog.GetTable("rolled_back")); });
        Reject([&] { static_cast<void>(catalog.GetIndex("rolled_back_idx")); });

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("CREATE TABLE kept (id INTEGER, name VARCHAR(20))");
        sql.ExecuteSQL("INSERT INTO kept VALUES (1, 'one')");
        sql.ExecuteSQL("CREATE INDEX kept_idx ON kept(id)");
        sql.ExecuteSQL("COMMIT");
        Check(sql.ExecuteSQL("SELECT name FROM kept WHERE id = 1").rows.size() == 1,
              "Committed CREATE TABLE or CREATE INDEX is not usable");

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("DROP INDEX kept_idx");
        sql.ExecuteSQL("ROLLBACK");
        Check(catalog.GetIndex("kept_idx").GetTree().GetValue(1).has_value(),
              "Rolled-back DROP INDEX did not restore the index");

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("DROP TABLE kept");
        sql.ExecuteSQL("ROLLBACK");
        Check(sql.ExecuteSQL("SELECT * FROM kept").rows.size() == 1 &&
                  catalog.GetIndex("kept_idx").GetTree().GetValue(1).has_value(),
              "Rolled-back DROP TABLE did not restore table and index state");

        database->Close();
    }
    {
        auto database = Database::Open(path, 3);
        SqlEngine sql(database->GetCatalog());
        Check(sql.ExecuteSQL("SELECT * FROM kept WHERE id = 1").rows.size() == 1,
              "Transactional DDL did not persist after reopen");
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("DROP TABLE kept");
        sql.ExecuteSQL("COMMIT");
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        Reject([&] { static_cast<void>(database->GetCatalog().GetTable("kept")); });
        Check(database->GetCatalog().ListIndexes().empty(),
              "Committed DROP TABLE left index metadata after reopen");
        database->Close();
    }
}

void TestSchemaLock(const std::filesystem::path& path) {
    auto database = Database::Create(path, 3);
    SqlEngine ddl(database->GetCatalog());
    SqlEngine reader(database->GetCatalog());
    ddl.ExecuteSQL("CREATE TABLE stable (id INTEGER)");
    ddl.ExecuteSQL("INSERT INTO stable VALUES (1)");
    ddl.ExecuteSQL("BEGIN");
    ddl.ExecuteSQL("CREATE TABLE pending (id INTEGER)");

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        started = true;
        const auto result = reader.ExecuteSQL("SELECT * FROM stable");
        Check(result.rows.size() == 1, "Concurrent reader returned wrong rows");
        finished = true;
    });
    while (!started.load()) { std::this_thread::yield(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Check(!finished.load(), "Schema lock allowed a query through unfinished DDL");
    ddl.ExecuteSQL("COMMIT");
    worker.join();
    Check(finished.load() && reader.ExecuteSQL("SELECT * FROM pending").rows.empty(),
          "Schema lock did not publish committed DDL");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-transactional-ddl-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestTransactionalDdl(directory / "ddl.udb");
        TestSchemaLock(directory / "locking.udb");
        std::cout << "Transactional DDL tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
