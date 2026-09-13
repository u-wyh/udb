#include "udb/database.h"
#include "udb/sql/engine.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

using ExpectedRows = std::map<std::int32_t, std::int32_t>;

void VerifyTable(Catalog& catalog, SqlEngine& sql, const std::string& table,
                 const std::string& index_name, const ExpectedRows& expected) {
    const auto rows = sql.ExecuteSQL("SELECT id, value FROM " + table + " ORDER BY id").rows;
    Check(rows.size() == expected.size(), "Recovered table row count is wrong");
    auto wanted = expected.begin();
    for (const auto& row : rows) {
        Check(wanted != expected.end() && row.GetValue(0).GetInteger() == wanted->first &&
              row.GetValue(1).GetInteger() == wanted->second,
              "Recovered table contents are wrong");
        ++wanted;
    }
    const auto table_id = catalog.GetTable(table).GetTableId();
    const auto& schema = catalog.GetTable(table_id).GetSchema();
    auto& tree = catalog.GetIndex(index_name).GetTree();
    tree.Validate();
    for (const auto& [id, value] : expected) {
        const auto rid = tree.GetValue(IndexKey(static_cast<std::int64_t>(id)));
        Check(rid.has_value(), "Recovered index missed a committed key");
        const auto tuple = Tuple::Deserialize(catalog.GetTableHeap(table_id).GetRecord(*rid), schema);
        Check(tuple.GetValue(0).GetInteger() == id && tuple.GetValue(1).GetInteger() == value,
              "Recovered index points to the wrong tuple");
    }
}

void RunConcurrentTransactions(Catalog& catalog, ExpectedRows& left, ExpectedRows& right) {
    constexpr int kWorkers = 6;
    constexpr int kRows = 12;
    std::atomic<int> ready = 0;
    std::atomic<bool> start = false;
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                SqlEngine sql(catalog, worker % 2 == 0 ? IsolationLevel::RepeatableRead
                                                       : IsolationLevel::ReadCommitted);
                ++ready;
                while (!start.load()) { std::this_thread::yield(); }
                sql.ExecuteSQL("BEGIN");
                const std::string table = worker % 2 == 0 ? "left_table" : "right_table";
                for (int row = 0; row < kRows; ++row) {
                    const auto id = 100 + worker * 20 + row;
                    const auto value = worker * 1000 + row;
                    sql.ExecuteSQL("INSERT INTO " + table + " VALUES (" +
                                   std::to_string(id) + ", " + std::to_string(value) + ", '" +
                                   std::string(300, static_cast<char>('a' + worker)) + "')");
                }
                sql.ExecuteSQL("UPDATE " + table + " SET value = " +
                               std::to_string(5000 + worker) + " WHERE id = " +
                               std::to_string(worker));
                if (worker == 2 || worker == 5) { sql.ExecuteSQL("ROLLBACK"); }
                else { sql.ExecuteSQL("COMMIT"); }
            } catch (...) { failed = true; }
        });
    }
    while (ready.load() != kWorkers) { std::this_thread::yield(); }
    start = true;
    for (auto& worker : workers) { worker.join(); }
    Check(!failed, "Concurrent transaction worker failed");

    for (int worker = 0; worker < kWorkers; ++worker) {
        if (worker == 2 || worker == 5) { continue; }
        auto& target = worker % 2 == 0 ? left : right;
        target[worker] = 5000 + worker;
        for (int row = 0; row < kRows; ++row) {
            target[100 + worker * 20 + row] = worker * 1000 + row;
        }
    }
}

void RunPostRecoveryTransactions(Catalog& catalog, ExpectedRows& left, ExpectedRows& right) {
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                SqlEngine sql(catalog);
                sql.ExecuteSQL("BEGIN");
                const bool use_left = worker % 2 == 0;
                const std::string table = use_left ? "left_table" : "right_table";
                const int base = (use_left ? 2000 : 3000) + worker * 20;
                for (int row = 0; row < 10; ++row) {
                    sql.ExecuteSQL("INSERT INTO " + table + " VALUES (" +
                                   std::to_string(base + row) + ", " +
                                   std::to_string(9000 + base + row) + ", 'recovered')");
                }
                sql.ExecuteSQL("COMMIT");
            } catch (...) { failed = true; }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    Check(!failed, "Post-recovery concurrent transaction failed");
    for (int worker = 0; worker < 4; ++worker) {
        auto& target = worker % 2 == 0 ? left : right;
        const int base = (worker % 2 == 0 ? 2000 : 3000) + worker * 20;
        for (int row = 0; row < 10; ++row) {
            target[base + row] = 9000 + base + row;
        }
    }
}

void TestConcurrentRecovery(const std::filesystem::path& path) {
    ExpectedRows left;
    ExpectedRows right;
    for (int id = 0; id < 6; ++id) {
        left[id] = id;
        right[id] = id;
    }
    {
        auto database = Database::Create(path, 4);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE left_table (id INTEGER, value INTEGER, note VARCHAR(400))");
        sql.ExecuteSQL("CREATE TABLE right_table (id INTEGER, value INTEGER, note VARCHAR(400))");
        for (int id = 0; id < 6; ++id) {
            sql.ExecuteSQL("INSERT INTO left_table VALUES (" + std::to_string(id) + ", " +
                           std::to_string(id) + ", 'base')");
            sql.ExecuteSQL("INSERT INTO right_table VALUES (" + std::to_string(id) + ", " +
                           std::to_string(id) + ", 'base')");
        }
        sql.ExecuteSQL("CREATE INDEX left_id ON left_table(id)");
        sql.ExecuteSQL("CREATE INDEX right_id ON right_table(id)");
        database->Close();
    }
    {
        auto database = Database::Open(path, 4);
        auto& catalog = database->GetCatalog();
        RunConcurrentTransactions(catalog, left, right);

        // BEGIN order is deliberately opposite to serialization/COMMIT order.
        SqlEngine began_first(catalog);
        SqlEngine committed_first(catalog);
        began_first.ExecuteSQL("BEGIN");
        committed_first.ExecuteSQL("BEGIN");
        committed_first.ExecuteSQL("UPDATE left_table SET value = 700 WHERE id = 0");
        committed_first.ExecuteSQL("COMMIT");
        bool conflicted = false;
        try { began_first.ExecuteSQL("UPDATE left_table SET value = 701 WHERE id = 0"); }
        catch (const WriteConflictError&) { conflicted = true; }
        Check(conflicted && !began_first.HasActiveTransaction(),
              "Stale writer did not abort on a first-committer conflict");
        left[0] = 700;

        SqlEngine loser(catalog);
        loser.ExecuteSQL("BEGIN");
        loser.ExecuteSQL("INSERT INTO right_table VALUES (999, 999, 'loser')");
        database->Flush();  // Put the loser images on disk before the simulated crash.
        SqlEngine later_commit(catalog);
        later_commit.ExecuteSQL("INSERT INTO left_table VALUES (888, 8880, 'redo')");
        left[888] = 8880;
        // Simulated crash: no ROLLBACK for loser and no Close for Database.
    }
    {
        auto database = Database::Open(path, 3);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        VerifyTable(catalog, sql, "left_table", "left_id", left);
        VerifyTable(catalog, sql, "right_table", "right_id", right);
        Check(!catalog.GetIndex("right_id").GetTree().GetValue(IndexKey(std::int64_t{999})),
              "Loser index key survived crash recovery");
        RunPostRecoveryTransactions(catalog, left, right);
        VerifyTable(catalog, sql, "left_table", "left_id", left);
        VerifyTable(catalog, sql, "right_table", "right_id", right);
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        VerifyTable(database->GetCatalog(), sql, "left_table", "left_id", left);
        VerifyTable(database->GetCatalog(), sql, "right_table", "right_id", right);
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-concurrent-recovery-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestConcurrentRecovery(directory / "database.udb");
        std::cout << "Concurrent recovery stress tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
