#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/cost_model.h"
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

std::unique_ptr<PlanNode> Plan(const Catalog& catalog, const std::string& sql, bool use_catalog = true) {
    const auto bound = Binder(catalog).Bind(Parser::Parse(sql));
    return use_catalog ? Planner::Plan(bound, catalog) : Planner::Plan(bound);
}

void TestCosts(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER, category INTEGER, name VARCHAR(20))");
    for (int i = 0; i < 100; ++i) {
        sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                       std::to_string(i % 5) + ", 'row')");
    }
    const auto table = catalog.GetTable("t").GetTableId();
    catalog.CreateIndex("id_idx", table, 0);
    catalog.CreateIndex("category_idx", table, 1, {3, 3, false});
    catalog.AnalyzeTable(table);

    const auto sequential = Plan(catalog, "SELECT name FROM t WHERE id = 50", false);
    const auto exact = Plan(catalog, "SELECT name FROM t WHERE id = 50");
    const auto covered = Plan(catalog, "SELECT id FROM t WHERE id = 50");
    const auto repeated = Plan(catalog, "SELECT name FROM t WHERE category = 2");
    const auto range = Plan(catalog, "SELECT name FROM t WHERE id >= 10");
    const auto limited = Plan(catalog, "SELECT name FROM t WHERE category = 2 LIMIT 2");

    const auto seq_cost = CostModel::Estimate(*sequential, catalog);
    const auto exact_cost = CostModel::Estimate(*exact, catalog);
    const auto covered_cost = CostModel::Estimate(*covered, catalog);
    const auto repeated_cost = CostModel::Estimate(*repeated, catalog);
    const auto range_cost = CostModel::Estimate(*range, catalog);
    const auto limited_cost = CostModel::Estimate(*limited, catalog);
    Check(seq_cost.total_cost == 100 && seq_cost.estimated_rows == 25,
          "SeqScan estimate is wrong");
    Check(exact_cost.estimated_rows == 1 && exact_cost.total_cost < seq_cost.total_cost,
          "Unique equality estimate is wrong");
    Check(covered_cost.estimated_rows == 1 && covered_cost.total_cost < exact_cost.total_cost,
          "Index-only cost is not cheaper than heap lookup");
    Check(repeated_cost.estimated_rows == 20 && repeated_cost.total_cost > exact_cost.total_cost,
          "Distinct-count equality estimate is wrong");
    Check(range_cost.estimated_rows > repeated_cost.estimated_rows &&
          range_cost.total_cost < seq_cost.total_cost, "Range estimate is wrong");
    Check(limited_cost.estimated_rows == 2 && limited_cost.total_cost < repeated_cost.total_cost,
          "LIMIT did not reduce index work");

    sql.ExecuteSQL("CREATE TABLE unknown (id INTEGER)");
    const auto fallback = Plan(catalog, "SELECT id FROM unknown", false);
    const auto fallback_cost = CostModel::Estimate(*fallback, catalog);
    Check(fallback_cost.total_cost == 1000 && fallback_cost.estimated_rows == 1000,
          "Missing-statistics fallback is wrong");
    const auto create = Plan(catalog, "CREATE TABLE later (id INTEGER)");
    bool rejected = false;
    try { CostModel::Estimate(*create, catalog); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Unsupported plan was assigned a scan cost");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-cost-model-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestCosts(directory / "cost.udb");
        std::cout << "Cost model tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
