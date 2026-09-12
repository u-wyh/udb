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

void TestSmallTable(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);
    sql.ExecuteSQL("CREATE TABLE tiny (id INTEGER, value VARCHAR(20))");
    sql.ExecuteSQL("INSERT INTO tiny VALUES (1, 'one')");
    sql.ExecuteSQL("INSERT INTO tiny VALUES (2, 'two')");
    sql.ExecuteSQL("CREATE INDEX tiny_id ON tiny(id)");
    Check(Plan(catalog, "SELECT value FROM tiny WHERE id = 1")->GetType() == PlanType::IndexScan,
          "Missing statistics no longer use the conservative index path");
    catalog.AnalyzeTable("tiny");
    Check(Plan(catalog, "SELECT value FROM tiny WHERE id = 1")->GetType() == PlanType::SeqScan,
          "Small-table heap lookup was not replaced by SeqScan");
    Check(Plan(catalog, "SELECT id FROM tiny WHERE id = 1")->GetType() == PlanType::IndexOnlyScan,
          "Small-table covering lookup lost IndexOnlyScan");
    Check(Plan(catalog, "SELECT value FROM tiny WHERE id >= 1")->GetType() == PlanType::SeqScan,
          "Small-table range lookup was not replaced by SeqScan");
    const auto result = sql.ExecuteSQL("SELECT value FROM tiny WHERE id = 1");
    Check(result.type == PlanType::SeqScan && result.rows.size() == 1 &&
          result.rows[0].GetValue(0) == Value::Varchar("one"),
          "Cost-selected SeqScan changed query results");
    database->Close();
}

void TestSelectivityAndCandidates(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);
    sql.ExecuteSQL("CREATE TABLE t (a INTEGER, b INTEGER, payload VARCHAR(20))");
    for (int i = 0; i < 100; ++i) {
        sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i % 2) + ", " +
                       std::to_string(i) + ", 'row')");
    }
    const auto table = catalog.GetTable("t").GetTableId();
    const auto a_index = catalog.CreateIndex("a_idx", table, 0, {3, 3, false})
                             .GetMetadata().GetIndexId();
    const auto b_index = catalog.CreateIndex("b_idx", table, 1)
                             .GetMetadata().GetIndexId();
    catalog.AnalyzeTable(table);

    Check(Plan(catalog, "SELECT payload FROM t WHERE a = 1")->GetType() == PlanType::SeqScan,
          "Low-cardinality index was chosen over SeqScan");
    const auto selective = Plan(catalog, "SELECT payload FROM t WHERE b = 42");
    Check(selective->GetType() == PlanType::IndexScan &&
          dynamic_cast<const IndexScanPlan&>(*selective).GetIndexId() == b_index,
          "Selective unique index was not chosen");
    const auto competing = Plan(catalog, "SELECT payload FROM t WHERE a = 0 AND b = 42");
    Check(competing->GetType() == PlanType::IndexScan &&
          dynamic_cast<const IndexScanPlan&>(*competing).GetIndexId() == b_index,
          "Cheapest equality candidate was not selected");
    Check(dynamic_cast<const IndexScanPlan&>(*competing).GetIndexId() != a_index,
          "Lower ID overrode the cheaper access path");
    Check(Plan(catalog, "SELECT payload FROM t WHERE b >= 90")->GetType() == PlanType::IndexRangeScan,
          "Useful range index was not selected");
    Check(sql.ExecuteSQL("SELECT payload FROM t WHERE a = 0 AND b = 42").rows.size() == 1,
          "Cost-selected index changed residual filtering");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-access-path-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestSmallTable(directory / "small.udb");
        TestSelectivityAndCandidates(directory / "selectivity.udb");
        std::cout << "Access path optimizer tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
