#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/transaction.h"

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

void TestPageWriteSets(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 2);
    TransactionManager transactions(&pool);

    const auto [existing_id, existing] = pool.NewPage();
    existing->data[0] = 'a';
    pool.UnpinPage(existing_id, true);
    pool.FlushAllPages();

    auto& write = transactions.Begin();
    pool.SetActiveTransaction(&write);
    {
        auto guard = pool.WritePage(existing_id);
        guard.GetPage().data[0] = 'b';
    }
    transactions.Abort(write);
    {
        auto guard = pool.ReadPage(existing_id);
        Check(guard.GetPage().data[0] == 'a', "Page before-image was not restored");
    }

    auto& allocation = transactions.Begin();
    pool.SetActiveTransaction(&allocation);
    page_id_t allocated_id = -1;
    {
        auto guard = pool.NewPageGuard();
        allocated_id = guard.GetPageId();
        guard.GetPage().data[0] = 'n';
    }
    transactions.Abort(allocation);
    Check(!disk.IsPageAllocated(allocated_id), "Rolled-back page allocation remained allocated");

    auto& freeing = transactions.Begin();
    pool.SetActiveTransaction(&freeing);
    Check(pool.DeletePage(existing_id), "Transaction page free failed");
    transactions.Abort(freeing);
    Check(disk.IsPageAllocated(existing_id), "Rolled-back page free remained free");
    {
        auto guard = pool.ReadPage(existing_id);
        Check(guard.GetPage().data[0] == 'a', "Freed page image was not restored");
    }
}

void CheckRows(SqlEngine& sql, const std::vector<std::int32_t>& ids) {
    const auto result = sql.ExecuteSQL("SELECT id FROM t ORDER BY id");
    Check(result.rows.size() == ids.size(), "Unexpected transaction row count");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        Check(result.rows[i].GetValue(0) == Value::Integer(ids[i]),
              "Transaction row contents are wrong");
    }
}

void TestSqlTransactions(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine sql(catalog);
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, name VARCHAR(2000))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'one')");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'two')");
        const auto table_id = catalog.GetTable("t").GetTableId();
        catalog.CreateIndex("idx_t_id", table_id, 0, BPlusTreeOptions{3, 3});

        const auto begun = sql.ExecuteSQL("  begin ; ");
        Check(begun.type == PlanType::Begin && begun.transaction_id && sql.HasActiveTransaction(),
              "BEGIN did not establish a transaction");
        sql.ExecuteSQL("INSERT INTO t VALUES (3, 'three')");
        sql.ExecuteSQL("UPDATE t SET id = 10, name = 'changed' WHERE id = 1");
        sql.ExecuteSQL("DELETE FROM t WHERE id = 2");
        for (int id = 20; id < 28; ++id) {
            sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(id) + ", '" +
                           std::string(1500, 'x') + "')");
        }
        Check(sql.ExecuteSQL("ROLLBACK").type == PlanType::Rollback && !sql.HasActiveTransaction(),
              "ROLLBACK did not end the transaction");
        CheckRows(sql, {1, 2});
        auto& tree = catalog.GetIndex("idx_t_id").GetTree();
        tree.Validate();
        Check(tree.GetValue(1) && tree.GetValue(2) && !tree.GetValue(3) && !tree.GetValue(10),
              "ROLLBACK did not restore index entries");

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("UPDATE t SET id = 11 WHERE id = 1");
        sql.ExecuteSQL("DELETE FROM t WHERE id = 2");
        sql.ExecuteSQL("INSERT INTO t VALUES (4, 'four')");
        Check(sql.ExecuteSQL("COMMIT;").type == PlanType::Commit && !sql.HasActiveTransaction(),
              "COMMIT did not end the transaction");
        CheckRows(sql, {4, 11});
        tree.Validate();

        sql.ExecuteSQL("BEGIN");
        Reject([&] { sql.ExecuteSQL("INSERT INTO t VALUES (11, 'duplicate')"); });
        Check(!sql.HasActiveTransaction(), "Constraint failure did not abort explicit transaction");
        CheckRows(sql, {4, 11});
        tree.Validate();

        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("CREATE TABLE rolled_back (id INTEGER)");
        Check(sql.HasActiveTransaction(), "Transactional DDL ended the explicit transaction");
        sql.ExecuteSQL("ROLLBACK");
        Reject([&] { static_cast<void>(catalog.GetTable("rolled_back")); });
        Reject([&] { sql.ExecuteSQL("COMMIT"); });
        Reject([&] { sql.ExecuteSQL("ROLLBACK"); });

        const auto automatic = sql.ExecuteSQL("INSERT INTO t VALUES (5, 'auto')");
        Check(!automatic.transaction_id && automatic.affected_rows == 1,
              "Autocommit changed the legacy result contract");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        CheckRows(sql, {4, 5, 11});
        auto& tree = database->GetCatalog().GetIndex("idx_t_id").GetTree();
        tree.Validate();
        Check(tree.GetValue(4) && tree.GetValue(5) && tree.GetValue(11) && !tree.GetValue(1),
              "Committed index state was not persistent");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-explicit-transaction-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestPageWriteSets(directory / "pages.udb");
        TestSqlTransactions(directory / "sql.udb");
        std::cout << "Explicit transaction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
