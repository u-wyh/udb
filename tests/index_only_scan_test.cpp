#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::unique_ptr<PlanNode> Plan(const Catalog& catalog, const std::string& sql) {
    return Planner::Plan(Binder(catalog).Bind(Parser::Parse(sql)), catalog);
}

void TestPlanning(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const auto table = catalog.CreateTable("t", Schema({Column("id", TypeId::INTEGER),
        Column("big", TypeId::BIGINT), Column("name", TypeId::VARCHAR, 20)})).GetTableId();
    catalog.CreateIndex("id_idx", table, 0);
    catalog.CreateIndex("big_idx", table, 1);
    catalog.CreateIndex("pair_idx", table, std::vector<std::size_t>{0, 1});

    Check(Plan(catalog, "SELECT id FROM t WHERE id = 1")->GetType() == PlanType::IndexOnlyScan,
          "Exact projection did not use IndexOnlyScan");
    Check(Plan(catalog, "SELECT id AS key FROM t WHERE 1 = id")->GetType() == PlanType::IndexOnlyScan,
          "Aliased column did not use IndexOnlyScan");
    Check(Plan(catalog, "SELECT big FROM t WHERE big >= 3 AND big < 9")->GetType() == PlanType::IndexOnlyScan,
          "Range projection did not use IndexOnlyScan");
    Check(Plan(catalog, "SELECT name FROM t WHERE id = 1")->GetType() == PlanType::IndexScan,
          "Non-covered projection used IndexOnlyScan");
    Check(Plan(catalog, "SELECT id FROM t WHERE id = 1 AND name = 'x'")->GetType() == PlanType::IndexScan,
          "Residual predicate used IndexOnlyScan");
    Check(Plan(catalog, "SELECT id FROM t WHERE id = 1 ORDER BY id")->GetType() == PlanType::IndexScan,
          "ORDER BY incorrectly used basic IndexOnlyScan");
    Check(Plan(catalog, "SELECT id FROM t WHERE id = 1 AND big = 2")->GetType() == PlanType::IndexScan,
          "Composite index incorrectly used basic IndexOnlyScan");
}

void TestExecutionAndPersistence(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, name VARCHAR(20))");
        sql.ExecuteSQL("CREATE INDEX big_idx ON t(big)");
        for (int i = 0; i < 30; ++i) {
            sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i % 5) + ", " +
                           std::to_string(3000000000LL + i) + ", 'row')");
        }
        const auto table = database->GetCatalog().GetTable("t").GetTableId();
        database->GetCatalog().CreateIndex("id_idx", table, 0, {3, 3, false});
        const auto exact = sql.ExecuteSQL("SELECT id FROM t WHERE id = 3 LIMIT 2 OFFSET 1");
        Check(exact.type == PlanType::IndexOnlyScan && exact.rows.size() == 2 &&
              exact.rows[0].GetValue(0) == Value::Integer(3),
              "Non-unique exact IndexOnlyScan result is wrong");
        const auto range = sql.ExecuteSQL(
            "SELECT big FROM t WHERE big >= 3000000005 AND big < 3000000010");
        Check(range.type == PlanType::IndexOnlyScan && range.rows.size() == 5 &&
              range.rows.front().GetValue(0) == Value::BigInt(3000000005LL) &&
              range.rows.back().GetValue(0) == Value::BigInt(3000000009LL),
              "BIGINT range IndexOnlyScan result is wrong");
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE id = NULL").rows.empty(),
              "NULL predicate returned indexed rows");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine sql(database->GetCatalog());
        const auto result = sql.ExecuteSQL("SELECT id FROM t WHERE id = 4");
        Check(result.type == PlanType::IndexOnlyScan && result.rows.size() == 6,
              "IndexOnlyScan failed after reopen");
        database->Close();
    }
}

void TestDoesNotReadHeap(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER)});
    const auto table = catalog.CreateTable("t", schema).GetTableId();
    const Tuple tuple(schema, {Value::Integer(7)});
    const auto rid = catalog.GetTableHeap(table).InsertRecord(tuple.Serialize(schema));
    catalog.CreateIndex("idx", table, 0);
    catalog.GetTableHeap(table).DeleteRecord(rid);
    Executor executor(catalog);
    const auto plan = Plan(catalog, "SELECT id FROM t WHERE id = 7");
    const auto result = executor.Execute(*plan);
    Check(result.type == PlanType::IndexOnlyScan && result.rows.size() == 1 &&
          result.rows[0].GetValue(0) == Value::Integer(7),
          "IndexOnlyScan accessed the stale heap RID");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-index-only-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestPlanning(directory / "planning.udb");
        TestExecutionAndPersistence(directory / "execution.udb");
        TestDoesNotReadHeap(directory / "stale.udb");
        std::cout << "Index-only scan tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
