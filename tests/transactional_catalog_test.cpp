#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void WithTransaction(Catalog& catalog, Transaction& transaction, Function function) {
    auto& pool = catalog.GetBufferPoolManager();
    pool.SetActiveTransaction(&transaction);
    try {
        function();
        pool.SetActiveTransaction(nullptr);
        pool.ThrowIfWriteError();
    } catch (...) {
        pool.SetActiveTransaction(nullptr);
        throw;
    }
}

void TestRollback(const std::filesystem::path& path) {
    auto database = Database::Create(path, 2);
    auto& catalog = database->GetCatalog();
    auto& transactions = catalog.GetTransactionManager();

    auto& create = transactions.Begin();
    WithTransaction(catalog, create, [&] {
        catalog.CreateTable("rolled_back", Schema({Column("id", TypeId::INTEGER)}));
    });
    Check(catalog.GetTable("rolled_back").GetTableId() == 0,
          "Transactional CREATE was not visible before rollback");
    transactions.Abort(create);
    try {
        static_cast<void>(catalog.GetTable("rolled_back"));
        throw std::runtime_error("Rolled-back CREATE remained in memory");
    } catch (const std::out_of_range&) {}

    SqlEngine sql(catalog);
    sql.ExecuteSQL("CREATE TABLE kept (id INTEGER)");
    sql.ExecuteSQL("INSERT INTO kept VALUES (7)");
    sql.ExecuteSQL("CREATE INDEX kept_idx ON kept(id)");
    const auto table_id = catalog.GetTable("kept").GetTableId();
    const auto index_id = catalog.GetIndex("kept_idx").GetMetadata().GetIndexId();
    const auto index_root = catalog.GetIndex(index_id).GetMetadata().GetHeaderPageId();

    auto& drop = transactions.Begin();
    WithTransaction(catalog, drop, [&] { catalog.DropTable(table_id); });
    transactions.Abort(drop);
    Check(catalog.GetTable(table_id).GetTableName() == "kept" &&
          catalog.GetIndex(index_id).GetMetadata().GetHeaderPageId() == index_root &&
          catalog.GetIndex(index_id).GetTree().GetValue(7).has_value(),
          "Rolled-back DROP did not restore table and index metadata");
    database->Close();

    database = Database::Open(path, 1);
    Check(database->GetCatalog().GetTable("kept").GetTableId() == table_id &&
          database->GetCatalog().GetIndex("kept_idx").GetTree().GetValue(7).has_value(),
          "Rolled-back catalog state was not durable");
    try {
        static_cast<void>(database->GetCatalog().GetTable("rolled_back"));
        throw std::runtime_error("Rolled-back CREATE reappeared after reopen");
    } catch (const std::out_of_range&) {}
    database->Close();
}

[[noreturn]] void CrashCatalogWrite(const std::filesystem::path& path, bool commit) {
    try {
        auto database = Database::Open(path, 2);
        auto& catalog = database->GetCatalog();
        if (commit) {
            SqlEngine sql(catalog);
            sql.ExecuteSQL("CREATE TABLE committed_after_crash (id BIGINT)");
        } else {
            auto& transaction = catalog.GetTransactionManager().Begin();
            WithTransaction(catalog, transaction, [&] {
                catalog.CreateTable("loser_after_crash", Schema({Column("id", TypeId::INTEGER)}));
            });
            database->GetLogManager().Flush();
            catalog.GetBufferPoolManager().FlushAllPages();
        }
        ::_exit(0);
    } catch (...) {
        ::_exit(1);
    }
}

void RunCrash(const std::filesystem::path& path, bool commit) {
    const auto child = ::fork();
    if (child < 0) { throw std::runtime_error("fork failed"); }
    if (child == 0) { CrashCatalogWrite(path, commit); }
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Catalog crash fixture failed");
}

void TestCrashRecovery(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 2);
        database->Close();
    }
    RunCrash(path, true);
    {
        auto database = Database::Open(path, 1);
        Check(database->GetCatalog().GetTable("committed_after_crash").GetSchema().GetColumn(0).GetType() ==
                  TypeId::BIGINT,
              "Committed catalog change was not redone after crash");
        database->Close();
    }
    RunCrash(path, false);
    {
        auto database = Database::Open(path, 1);
        Check(database->GetCatalog().GetTable("committed_after_crash").GetTableName() ==
                  "committed_after_crash",
              "Loser recovery damaged committed catalog state");
        try {
            static_cast<void>(database->GetCatalog().GetTable("loser_after_crash"));
            throw std::runtime_error("Loser catalog change survived crash recovery");
        } catch (const std::out_of_range&) {}
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-transactional-catalog-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestRollback(directory / "rollback.udb");
        TestCrashRecovery(directory / "crash.udb");
        std::cout << "Transactional catalog tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
