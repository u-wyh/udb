#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace udb;
using namespace udb::sql;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestRepeatableReadAndCommitRelease(Catalog& catalog) {
    SqlEngine reader(catalog);
    SqlEngine writer(catalog);
    reader.ExecuteSQL("BEGIN");
    auto rows = reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows;
    Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(10),
          "Initial repeatable-read value is wrong");

    std::atomic<bool> completed = false;
    std::atomic<bool> failed = false;
    std::thread blocked([&] {
        try {
            writer.ExecuteSQL("UPDATE t SET value = 20 WHERE id = 1");
            completed = true;
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(40ms);
    Check(!completed && !failed, "Writer bypassed a retained SELECT shared lock");
    rows = reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows;
    Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(10),
          "Repeatable read changed inside a transaction");
    reader.ExecuteSQL("COMMIT");
    blocked.join();
    Check(completed && !failed, "COMMIT did not release retained locks");
    rows = reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows;
    Check(rows[0].GetValue(0) == Value::Integer(20), "Committed writer value is missing");
}

void TestWriteLockAndRollbackRelease(Catalog& catalog) {
    SqlEngine writer(catalog);
    SqlEngine reader(catalog);
    writer.ExecuteSQL("BEGIN");
    writer.ExecuteSQL("UPDATE t SET value = 99 WHERE id = 1");

    std::atomic<bool> completed = false;
    std::atomic<bool> failed = false;
    std::int32_t observed = -1;
    std::thread blocked([&] {
        try {
            const auto rows = reader.ExecuteSQL("SELECT value FROM t WHERE id = 1").rows;
            observed = rows.at(0).GetValue(0).GetInteger();
            completed = true;
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(40ms);
    Check(!completed && !failed, "Reader observed an uncommitted write");
    writer.ExecuteSQL("ROLLBACK");
    blocked.join();
    Check(completed && !failed && observed == 20,
          "ROLLBACK did not restore data before releasing locks");
}

void TestIndependentTables(Catalog& catalog) {
    std::atomic<bool> failed = false;
    std::thread left([&] {
        try {
            SqlEngine sql(catalog);
            for (int i = 0; i < 40; ++i) {
                sql.ExecuteSQL("INSERT INTO left_table VALUES (" + std::to_string(i) + ")");
            }
        } catch (...) { failed = true; }
    });
    std::thread right([&] {
        try {
            SqlEngine sql(catalog);
            for (int i = 0; i < 40; ++i) {
                sql.ExecuteSQL("INSERT INTO right_table VALUES (" + std::to_string(i) + ")");
            }
        } catch (...) { failed = true; }
    });
    left.join();
    right.join();
    SqlEngine sql(catalog);
    Check(!failed && sql.ExecuteSQL("SELECT * FROM left_table").rows.size() == 40 &&
          sql.ExecuteSQL("SELECT * FROM right_table").rows.size() == 40,
          "Independent table transactions interfered with each other");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-strict-2pl-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 4);
        SqlEngine setup(database->GetCatalog());
        setup.ExecuteSQL("CREATE TABLE t (id INTEGER, value INTEGER)");
        setup.ExecuteSQL("INSERT INTO t VALUES (1, 10)");
        setup.ExecuteSQL("CREATE TABLE left_table (id INTEGER)");
        setup.ExecuteSQL("CREATE TABLE right_table (id INTEGER)");
        TestRepeatableReadAndCommitRelease(database->GetCatalog());
        TestWriteLockAndRollbackRelease(database->GetCatalog());
        TestIndependentTables(database->GetCatalog());
        database->Close();
        std::cout << "Strict 2PL tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
