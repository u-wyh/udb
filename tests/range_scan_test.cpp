#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

RID Expected(std::int64_t key) {
    return RID{key + 1000, static_cast<slot_id_t>((key + 1000) & 0xffff)};
}

void CheckKeys(const std::vector<std::pair<std::int64_t, RID>>& entries,
               std::int64_t first, std::int64_t last) {
    const auto expected_size = first <= last ? static_cast<std::size_t>(last - first + 1) : 0;
    Check(entries.size() == expected_size, "B+ tree range size is wrong");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto key = first + static_cast<std::int64_t>(i);
        Check(entries[i].first == key && entries[i].second == Expected(key),
              "B+ tree range entry is wrong");
    }
}

void TestTreeRanges(const std::filesystem::path& path) {
    page_id_t header_page_id;
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::CreateWithHeader(pool, BPlusTreeOptions{4, 3});
        header_page_id = tree->GetHeaderPageId();
        std::vector<std::int64_t> keys(101);
        std::iota(keys.begin(), keys.end(), -50);
        std::mt19937_64 random(0x55444222);
        std::shuffle(keys.begin(), keys.end(), random);
        for (const auto key : keys) { Check(tree->Insert(key, Expected(key)), "Range tree insert failed"); }
        CheckKeys(tree->ScanRange(std::nullopt, false, std::nullopt, false), -50, 50);
        CheckKeys(tree->ScanRange(-10, true, 10, true), -10, 10);
        CheckKeys(tree->ScanRange(-10, false, 10, false), -9, 9);
        CheckKeys(tree->ScanRange(std::nullopt, false, -45, true), -50, -45);
        CheckKeys(tree->ScanRange(45, true, std::nullopt, false), 45, 50);
        CheckKeys(tree->ScanRange(7, true, 7, true), 7, 7);
        Check(tree->ScanRange(7, false, 7, true).empty() &&
              tree->ScanRange(8, true, 7, true).empty(), "Empty B+ tree range returned rows");
        for (std::int64_t key = -5; key <= 5; ++key) { Check(tree->Remove(key), "Range delete failed"); }
        const auto after_delete = tree->ScanRange(-10, true, 10, true);
        Check(after_delete.size() == 10 && after_delete.front().first == -10 &&
              after_delete.back().first == 10, "Range scan included deleted keys");
        tree->Validate();
        pool.FlushAllPages();
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::OpenWithHeader(pool, header_page_id);
        CheckKeys(tree->ScanRange(40, false, 50, true), 41, 50);
        Check(tree->ScanRange(-5, true, 5, true).empty(), "Deleted range returned after reopen");
        tree->Validate();
    }
}

std::unique_ptr<PlanNode> MakePlan(const Catalog& catalog, const std::string& sql) {
    return Planner::Plan(Binder(catalog).Bind(Parser::Parse(sql)), catalog);
}

void CheckSameRows(const ExecutionResult& indexed, const ExecutionResult& sequential) {
    Check(indexed.rows.size() == sequential.rows.size() &&
          indexed.output_schema.GetColumnCount() == sequential.output_schema.GetColumnCount(),
          "Range scan and SeqScan shapes differ");
    for (std::size_t row = 0; row < indexed.rows.size(); ++row) {
        for (std::size_t column = 0; column < indexed.output_schema.GetColumnCount(); ++column) {
            Check(indexed.rows[row].GetValue(column) == sequential.rows[row].GetValue(column),
                  "Range scan and SeqScan values differ");
        }
    }
}

void TestPlanningAndExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL(
            "CREATE TABLE t (id INTEGER, big BIGINT, group_id INTEGER, payload VARCHAR(900))");
        engine.ExecuteSQL(
            "CREATE TABLE plain (id INTEGER, big BIGINT, group_id INTEGER, payload VARCHAR(900))");
        for (int i = 0; i < 30; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                std::to_string(3000000000LL + i) + ", " + std::to_string(i % 2) + ", '" +
                std::string(700, static_cast<char>('a' + i % 20)) + "')");
        }
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, 0, 'null')");
        engine.ExecuteSQL("CREATE INDEX idx_id ON t(id)");
        engine.ExecuteSQL("CREATE INDEX idx_big ON t(big)");

        const auto bounded = MakePlan(catalog, "SELECT big, id FROM t WHERE id >= 5 AND id < 10");
        const auto& range = dynamic_cast<const IndexRangeScanPlan&>(*bounded);
        Check(range.GetLowerBound() == 5 && range.IsLowerInclusive() &&
              range.GetUpperBound() == 10 && !range.IsUpperInclusive() &&
              range.GetColumnIndexes() == std::vector<std::size_t>({1, 0}),
              "Bounded range plan is wrong");
        const auto reverse = MakePlan(catalog, "SELECT id FROM t WHERE 10 > id");
        const auto& reverse_range = dynamic_cast<const IndexOnlyScanPlan&>(*reverse);
        Check(!reverse_range.GetLowerBound() && reverse_range.GetUpperBound() == 10 &&
              !reverse_range.IsUpperInclusive(), "Reversed range plan is wrong");
        Check(MakePlan(catalog, "SELECT * FROM plain WHERE id > 1")->GetType() == PlanType::SeqScan &&
              MakePlan(catalog, "SELECT * FROM t WHERE id != 1")->GetType() == PlanType::SeqScan &&
              MakePlan(catalog, "SELECT * FROM t WHERE id > 1 OR id < 0")->GetType() == PlanType::SeqScan &&
              MakePlan(catalog, "SELECT * FROM t WHERE id > NULL")->GetType() == PlanType::SeqScan,
              "Unsafe range predicate selected an index");

        auto result = engine.ExecuteSQL("SELECT big, id FROM t WHERE id >= 5 AND id < 10");
        Check(result.type == PlanType::IndexRangeScan && result.rows.size() == 5,
              "Bounded IndexRangeScan row count is wrong");
        for (std::size_t i = 0; i < result.rows.size(); ++i) {
            Check(result.rows[i].GetValue(0) == Value::BigInt(3000000005LL + static_cast<std::int64_t>(i)) &&
                  result.rows[i].GetValue(1) == Value::Integer(5 + static_cast<std::int32_t>(i)),
                  "Range projection or key order is wrong");
        }
        Check(engine.ExecuteSQL("SELECT id FROM t WHERE id > 28").rows.size() == 1 &&
              engine.ExecuteSQL("SELECT id FROM t WHERE id <= 1").rows.size() == 2 &&
              engine.ExecuteSQL("SELECT id FROM t WHERE 28 < id").rows.size() == 1,
              "One-sided or reversed range result is wrong");
        result = engine.ExecuteSQL(
            "SELECT id FROM t WHERE big >= 3000000010 AND big <= 3000000012");
        Check(result.type == PlanType::IndexRangeScan && result.rows.size() == 3 &&
              result.rows.front().GetValue(0) == Value::Integer(10), "BIGINT range scan is wrong");
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 5 AND id < 10 AND group_id = 1");
        Check(result.rows.size() == 3 && result.rows[0].GetValue(0) == Value::Integer(5) &&
              result.rows[2].GetValue(0) == Value::Integer(9), "Range residual filter is wrong");

        const auto bound = Binder(catalog).Bind(Parser::Parse(
            "SELECT big, id FROM t WHERE id >= 15 AND id <= 18"));
        Executor executor(catalog);
        CheckSameRows(executor.Execute(*Planner::Plan(bound, catalog)),
                      executor.Execute(*Planner::Plan(bound)));

        engine.ExecuteSQL("UPDATE t SET id = 105 WHERE id = 5");
        engine.ExecuteSQL("DELETE FROM t WHERE id = 6");
        engine.ExecuteSQL("INSERT INTO t VALUES (55, 4000000000, 1, 'new')");
        result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 5 AND id <= 7");
        Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == Value::Integer(7),
              "Range scan did not observe UPDATE/DELETE maintenance");
        Check(engine.ExecuteSQL("SELECT id FROM t WHERE id >= 50 AND id < 60").rows.size() == 1,
              "Range scan did not observe INSERT maintenance");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        const auto result = engine.ExecuteSQL("SELECT id FROM t WHERE id >= 100 AND id <= 110");
        Check(result.type == PlanType::IndexOnlyScan && result.rows.size() == 1 &&
              result.rows[0].GetValue(0) == Value::Integer(105),
              "Range scan failed after Close/Open");
        Check(engine.ExecuteSQL("SELECT id FROM t WHERE id >= 5 AND id <= 7").rows.size() == 1,
              "Persistent range scan disagrees with DML state");
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
                               ("udb-range-scan-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestTreeRanges(directory / "tree.udb");
        TestPlanningAndExecution(directory / "database.udb");
        std::cout << "Range scan tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
