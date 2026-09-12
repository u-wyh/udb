#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/sql/binder.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}
template <typename Function> void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected composite constraint failure");
}

void TestEncoding() {
    const auto key = [](std::int64_t n, std::string s) { return MakeCompositeKey({IndexKey(n), IndexKey(std::move(s))}); };
    Check(key(-2, "z") < key(-1, "a") && key(-1, "z") < key(0, "") && key(0, "") < key(1, ""),
          "Signed component ordering failed");
    Check(key(1, "a") < key(1, std::string("a\0", 2)), "Embedded zero ordering failed");
    Check(MakeCompositeKey({IndexKey("a"), IndexKey("bc")}) !=
          MakeCompositeKey({IndexKey("ab"), IndexKey("c")}), "Component boundary collision");
}

void Verify(Database& database) {
    auto& catalog = database.GetCatalog();
    SqlEngine sql(catalog);
    const auto& index = catalog.GetIndex("ab");
    Check(index.GetMetadata().GetColumnIndexes() == std::vector<std::size_t>({0, 1}), "Column order did not persist");
    index.GetTree().Validate();
    catalog.GetIndex("ac").GetTree().Validate();
    const auto result = sql.ExecuteSQL("SELECT b FROM t WHERE b = 'y' AND 1 = a");
    Check(result.type == PlanType::IndexScan && result.rows.size() == 1, "Full equality not matched");
    const auto bound = Binder(catalog).Bind(Parser::Parse("SELECT b FROM t WHERE b = 'y' AND 1 = a"));
    const auto plan = Planner::Plan(bound, catalog);
    Check(dynamic_cast<const IndexScanPlan&>(*plan).GetIndexId() == index.GetMetadata().GetIndexId(),
          "Planner preferred a shorter index over a full composite key");
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1").type == PlanType::IndexScan, "Single-column index was not used");
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a > 0").type == PlanType::IndexRangeScan,
          "Single-column range index was not used");
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 OR b = 'y'").type == PlanType::SeqScan, "OR misused composite index");
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 9 AND b = 'missing'").rows.empty(), "Missing composite key matched");
}

void Run(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (a INTEGER, b VARCHAR(20), c VARCHAR(20))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'x', 'p')");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'y', 'p')");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'x', NULL)");
        sql.ExecuteSQL("INSERT INTO t VALUES (NULL, 'x', 'p')");
        auto& catalog = database->GetCatalog();
        catalog.CreateIndex("a_only", catalog.GetTable("t").GetTableId(), 0, {3, 3, false});
        sql.ExecuteSQL("CREATE INDEX ab ON t(a, b)");
        catalog.CreateIndex("ac", catalog.GetTable("t").GetTableId(), std::vector<std::size_t>{0, 2}, {3, 3, false});
        Verify(*database);
        Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 AND c = 'p'").rows.size() == 2,
              "Non-unique composite query failed");
        Reject([&] { sql.ExecuteSQL("INSERT INTO t VALUES (1, 'x', 'other')"); });
        Reject([&] { sql.ExecuteSQL("CREATE INDEX duplicate_column ON t(a, a)"); });
        Reject([&] { sql.ExecuteSQL("CREATE INDEX duplicate_index ON t(a, b)"); });
        Reject([&] { sql.ExecuteSQL("CREATE INDEX missing_column ON t(a, missing)"); });
        sql.ExecuteSQL("UPDATE t SET b = 'z' WHERE a = 1 AND b = 'x'");
        Reject([&] { sql.ExecuteSQL("UPDATE t SET b = 'y' WHERE a = 1 AND b = 'z'"); });
        Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 AND b = 'z'").rows.size() == 1,
              "Constraint failure changed original key");
        sql.ExecuteSQL("UPDATE t SET c = 'q' WHERE a = 1");
        Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 AND c = 'q'").rows.size() == 2,
              "UPDATE did not maintain all composite entries");
        sql.ExecuteSQL("UPDATE t SET a = NULL WHERE a = 1 AND b = 'z'");
        Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 AND b = 'z'").rows.empty(), "NULL transition left an entry");
        sql.ExecuteSQL("UPDATE t SET a = 3 WHERE b = 'z'");
        Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 3 AND b = 'z'").rows.size() == 1, "NULL to value failed");
        sql.ExecuteSQL("CREATE TABLE wide (a VARCHAR(600), b VARCHAR(600))");
        Reject([&] { sql.ExecuteSQL("CREATE INDEX wide_key ON wide(a, b)"); });
        Verify(*database);
        database->Close();
    }
    auto database = Database::Open(path, 1);
    Verify(*database);
    SqlEngine sql(database->GetCatalog());
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 3 AND b = 'z'").rows.size() == 1, "Reopen composite lookup failed");
    sql.ExecuteSQL("DELETE FROM t WHERE a = 3 AND b = 'z'");
    Check(sql.ExecuteSQL("SELECT b FROM t WHERE a = 3 AND b = 'z'").rows.empty(), "Composite delete failed");
    sql.ExecuteSQL("DROP INDEX ab");
    const auto fallback = sql.ExecuteSQL("SELECT b FROM t WHERE a = 1 AND b = 'y'");
    Check(fallback.type == PlanType::IndexScan && fallback.rows.size() == 1,
          "Dropped composite index did not fall back to shorter index");
    sql.ExecuteSQL("DROP TABLE t");
    database->Close();
}
}  // namespace

int main() {
    try {
        TestEncoding();
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-composite-index-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        Run(directory / "database.udb");
        std::cout << "Composite index tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
