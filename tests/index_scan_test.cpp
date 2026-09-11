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

std::unique_ptr<PlanNode> MakePlan(const Catalog& catalog, const std::string& sql) {
    return Planner::Plan(Binder(catalog).Bind(Parser::Parse(sql)), catalog);
}

void CheckPlanType(const Catalog& catalog, const std::string& sql, PlanType expected) {
    Check(MakePlan(catalog, sql)->GetType() == expected, "Planner chose the wrong scan type");
}

void TestPlanning(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER), Column("big", TypeId::BIGINT),
                         Column("active", TypeId::BOOLEAN), Column("name", TypeId::VARCHAR, 20)});
    const auto table_id = catalog.CreateTable("t", schema).GetTableId();
    catalog.CreateTable("plain", schema);
    const auto id_index = catalog.CreateIndex("idx_id", table_id, 0).GetMetadata().GetIndexId();
    const auto big_index = catalog.CreateIndex("idx_big", table_id, 1).GetMetadata().GetIndexId();

    const auto direct = MakePlan(catalog, "SELECT name, id FROM t WHERE id = 123");
    const auto& id_scan = dynamic_cast<const IndexScanPlan&>(*direct);
    Check(id_scan.GetIndexId() == id_index && id_scan.GetKey() == 123 &&
          id_scan.GetColumnIndexes() == std::vector<std::size_t>({3, 0}) && id_scan.GetPredicate(),
          "INTEGER IndexScan plan payload is wrong");
    const auto reversed = MakePlan(catalog, "SELECT * FROM t WHERE 3000000000 = big");
    const auto& big_scan = dynamic_cast<const IndexScanPlan&>(*reversed);
    Check(big_scan.GetIndexId() == big_index && big_scan.GetKey() == 3000000000LL,
          "BIGINT reversed equality was not planned as IndexScan");

    CheckPlanType(catalog, "SELECT * FROM plain WHERE id = 1", PlanType::SeqScan);
    CheckPlanType(catalog, "SELECT * FROM t", PlanType::SeqScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id != 1", PlanType::SeqScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id < 1", PlanType::IndexRangeScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id <= 1", PlanType::IndexRangeScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id > 1", PlanType::IndexRangeScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id >= 1", PlanType::IndexRangeScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id = NULL", PlanType::SeqScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE id = 1 OR id = 2", PlanType::SeqScan);
    CheckPlanType(catalog, "SELECT * FROM t WHERE name = 'x'", PlanType::SeqScan);

    const auto conjunction = MakePlan(catalog,
        "SELECT id FROM t WHERE big = 3000000001 AND id = 1 AND name = 'x'");
    const auto& chosen = dynamic_cast<const IndexScanPlan&>(*conjunction);
    Check(chosen.GetIndexId() == id_index && chosen.GetKey() == 1 && chosen.GetPredicate(),
          "Multiple-index choice is not deterministic by index ID");
    const auto bound = Binder(catalog).Bind(Parser::Parse("SELECT * FROM t WHERE id = 1"));
    Check(Planner::Plan(bound)->GetType() == PlanType::SeqScan,
          "Catalog-free Planner unexpectedly selected an index");
}

void CheckSameRows(const ExecutionResult& indexed, const ExecutionResult& sequential) {
    Check(indexed.rows.size() == sequential.rows.size(), "IndexScan and SeqScan row counts differ");
    Check(indexed.output_schema.GetColumnCount() == sequential.output_schema.GetColumnCount(),
          "IndexScan and SeqScan tuple widths differ");
    for (std::size_t row = 0; row < indexed.rows.size(); ++row) {
        for (std::size_t column = 0; column < indexed.output_schema.GetColumnCount(); ++column) {
            Check(indexed.rows[row].GetValue(column) == sequential.rows[row].GetValue(column),
                  "IndexScan and SeqScan values differ");
        }
    }
}

void TestExecutionAndPersistence(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, name VARCHAR(1200))");
        for (int i = 0; i < 12; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                std::to_string(3000000000LL + i) + ", '" +
                std::string(900, static_cast<char>('a' + i)) + "')");
        }
        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        engine.ExecuteSQL("CREATE INDEX idx_big ON t(big)");
        const auto first_page = catalog.GetTable("t").GetFirstPageId();
        const auto rid_five = catalog.GetIndex("idx_id").GetTree().GetValue(5);
        Check(rid_five && rid_five->page_id != first_page,
              "IndexScan fixture did not place the target on a later table page");

        auto result = engine.ExecuteSQL("SELECT name, id FROM t WHERE id = 5");
        Check(result.type == PlanType::IndexScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Varchar(std::string(900, 'f')) &&
              result.rows[0].GetValue(1) == Value::Integer(5),
              "INTEGER IndexScan projection is wrong");
        Check(engine.ExecuteSQL("SELECT id FROM t WHERE 5 = id").type == PlanType::IndexScan,
              "Reversed INTEGER equality did not execute as IndexScan");
        result = engine.ExecuteSQL("SELECT id FROM t WHERE big = 3000000005");
        Check(result.type == PlanType::IndexScan && result.rows.at(0).GetValue(0) == Value::Integer(5),
              "BIGINT IndexScan returned the wrong row");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id = 999").rows.empty(),
              "Missing index key returned a row");

        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, 'null')");
        result = engine.ExecuteSQL("SELECT * FROM t WHERE id = NULL");
        Check(result.type == PlanType::SeqScan && result.rows.empty(), "NULL equality semantics changed");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id > 5").type == PlanType::IndexRangeScan &&
              engine.ExecuteSQL("SELECT * FROM t WHERE id = 5 OR id = 6").type == PlanType::SeqScan,
              "Range/OR predicate used the wrong scan path");
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id = 5 AND name = 'wrong'");
        Check(result.type == PlanType::IndexScan && result.rows.empty(), "Residual predicate was not evaluated");

        const auto bound = Binder(catalog).Bind(Parser::Parse("SELECT name, id FROM t WHERE id = 7"));
        Executor executor(catalog);
        const auto indexed = executor.Execute(*Planner::Plan(bound, catalog));
        const auto sequential = executor.Execute(*Planner::Plan(bound));
        CheckSameRows(indexed, sequential);

        engine.ExecuteSQL("UPDATE t SET id = 105 WHERE id = 5");
        Check(engine.ExecuteSQL("SELECT big FROM t WHERE id = 5").rows.empty() &&
              engine.ExecuteSQL("SELECT big FROM t WHERE id = 105").rows.at(0).GetValue(0) ==
                  Value::BigInt(3000000005LL),
              "IndexScan did not observe UPDATE maintenance");
        engine.ExecuteSQL("DELETE FROM t WHERE id = 6");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id = 6").rows.empty(),
              "IndexScan did not observe DELETE maintenance");
        const auto inserted = engine.ExecuteSQL("INSERT INTO t VALUES (100, 4000000000, 'new')");
        Check(engine.ExecuteSQL("SELECT big FROM t WHERE id = 100").rows.at(0).GetValue(0) ==
                  Value::BigInt(4000000000LL) && inserted.inserted_rid,
              "IndexScan did not observe INSERT maintenance");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        const auto result = engine.ExecuteSQL("SELECT id, name FROM t WHERE big = 3000000005");
        Check(result.type == PlanType::IndexScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Integer(105) &&
              result.rows[0].GetValue(1) == Value::Varchar(std::string(900, 'f')),
              "IndexScan failed after Close/Open");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id = 6").rows.empty() &&
              engine.ExecuteSQL("SELECT big FROM t WHERE id = 100").rows.size() == 1,
              "Reopened IndexScan disagrees with persistent DML state");
        catalog.GetIndex("idx_id").GetTree().Validate();
        catalog.GetIndex("idx_big").GetTree().Validate();
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-index-scan-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestPlanning(directory / "planning.udb");
        TestExecutionAndPersistence(directory / "execution.udb");
        std::cout << "Index scan tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
