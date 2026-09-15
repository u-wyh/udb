#include "udb/database.h"
#include "udb/sql/engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <typename F> void Reject(F f) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("Expected failure"); }

void Verify(SqlEngine& sql) {
    const auto rows = sql.ExecuteSQL("SELECT id, balance FROM accounts ORDER BY id").rows;
    Check(rows.size() == 3 && rows[0].GetValue(1).GetInteger() == 10 &&
          rows[1].GetValue(1).IsNull() && rows[2].GetValue(1).GetInteger() == 20,
          "CHECK fixture rows changed");
}

void TestChecks(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 3);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("BEGIN");
        sql.ExecuteSQL("CREATE TABLE discarded (v INTEGER CHECK (v > 0))");
        sql.ExecuteSQL("ROLLBACK");
        Reject([&] { static_cast<void>(database->GetCatalog().GetTable("discarded")); });

        sql.ExecuteSQL("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER CHECK (balance >= 0), CHECK (balance <= 100))");
        const auto& checks = database->GetCatalog().GetTable("accounts").GetSchema().GetCheckExpressions();
        Check(checks.size() == 2, "Column/table CHECK metadata is missing");
        sql.ExecuteSQL("INSERT INTO accounts VALUES (1, 10)");
        sql.ExecuteSQL("INSERT INTO accounts VALUES (2, NULL)");
        sql.ExecuteSQL("INSERT INTO accounts VALUES (3, 20)");
        Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (4, -1)"); });
        Reject([&] { sql.ExecuteSQL("INSERT INTO accounts VALUES (4, 101)"); });
        Verify(sql);
        Reject([&] { sql.ExecuteSQL("UPDATE accounts SET balance = -5 WHERE id >= 1"); });
        Verify(sql);
        Reject([&] { sql.ExecuteSQL("CREATE TABLE invalid (v INTEGER, CHECK (missing > 0))"); });
        Reject([&] { sql.ExecuteSQL("CREATE TABLE invalid_type (v INTEGER, CHECK (v + 1))"); });
        database->Close();
    }
    {
        auto database = Database::Open(path, 2);
        SqlEngine sql(database->GetCatalog());
        Check(database->GetCatalog().GetTable("accounts").GetSchema().GetCheckExpressions().size() == 2,
              "CHECK metadata was not restored");
        Reject([&] { sql.ExecuteSQL("UPDATE accounts SET balance = 200 WHERE id = 1"); });
        sql.ExecuteSQL("UPDATE accounts SET balance = NULL WHERE id = 1");
        Check(sql.ExecuteSQL("SELECT * FROM accounts WHERE id = 1").rows[0].GetValue(1).IsNull(),
              "SQL NULL did not satisfy CHECK");
        database->Close();
    }
}
}

int main() {
    try {
        const auto dir = std::filesystem::temp_directory_path() / ("udb-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Check(std::filesystem::create_directory(dir), "Cannot create test directory");
        struct Cleanup { std::filesystem::path p; ~Cleanup(){ std::error_code e; std::filesystem::remove_all(p,e); } } cleanup{dir};
        TestChecks(dir / "checks.udb");
        std::cout << "CHECK constraint tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
