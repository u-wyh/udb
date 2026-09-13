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

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

void TestParser() {
    const auto all = std::get<DeleteStatement>(Parser::Parse("DELETE FROM t;"));
    Check(all.table_name == "t" && !all.predicate, "DELETE all AST wrong");
    const auto filtered = std::get<DeleteStatement>(Parser::Parse("DELETE FROM t WHERE id = 1"));
    Check(filtered.table_name == "t" && filtered.predicate &&
          std::holds_alternative<ComparisonExpression>(filtered.predicate->node), "DELETE predicate AST wrong");
    const auto complex = std::get<DeleteStatement>(Parser::Parse(
        "DELETE FROM t WHERE id = 1 OR active = FALSE AND NOT name = 'x'"));
    Check(std::get<LogicalExpression>(complex.predicate->node).op == LogicalOperator::Or,
          "DELETE expression precedence wrong");
    for (const auto sql : {"DELETE", "DELETE t", "DELETE FROM", "DELETE FROM t WHERE",
                           "DELETE FROM t WHERE id =", "DELETE FROM t extra", "DELETE FROM t;;"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }
}

void TestBindingAndPlanning(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER), Column("active", TypeId::BOOLEAN)});
    const auto& table = catalog.CreateTable("t", schema);
    const auto tables = catalog.ListTables();
    const Binder binder(catalog);
    const auto all = std::get<BoundDeleteStatement>(binder.Bind(Parser::Parse("DELETE FROM t")));
    Check(all.table_id == table.GetTableId() && all.table_name == "t" && !all.predicate &&
          all.schema.GetColumnCount() == 2, "DELETE all binding wrong");
    const auto filtered = std::get<BoundDeleteStatement>(binder.Bind(
        Parser::Parse("DELETE FROM t WHERE id = 1 AND active = TRUE")));
    const auto& logical = std::get<BoundLogicalExpression>(filtered.predicate->node);
    const auto& comparison = std::get<BoundComparisonExpression>(logical.left->node);
    Check(std::get<BoundColumnExpression>(comparison.left->node).column_index == 0,
          "DELETE column was not bound to index");
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DELETE FROM missing")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DELETE FROM t WHERE missing = 1")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DELETE FROM t WHERE id = TRUE")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("DELETE FROM t WHERE id")); });
    const auto plan_node = Planner::Plan(BoundStatement{filtered});
    const auto& plan = dynamic_cast<const DeletePlan&>(*plan_node);
    Check(plan.GetType() == PlanType::Delete && plan.GetTableId() == table.GetTableId() &&
          plan.GetPredicate() && plan.GetOutputSchema().GetColumnCount() == 0,
          "DELETE plan wrong");
    Check(catalog.ListTables() == tables && !catalog.GetTableHeap("t").GetFirstRID(),
          "Binder or Planner executed DELETE");
}

void TestExecution(const std::filesystem::path& path) {
    std::vector<RID> rids;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(2000))");
        engine.ExecuteSQL("CREATE TABLE types (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(20))");
        for (int i = 0; i < 12; ++i) {
            const auto result = engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) +
                ", 2147484000, " + (i % 2 == 0 ? std::string("TRUE") : std::string("FALSE")) +
                ", '" + std::string(1400, static_cast<char>('a' + i)) + "')");
            Check(result.inserted_rid.has_value(), "INSERT did not return RID");
            rids.push_back(*result.inserted_rid);
        }
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL, NULL)");
        Check(rids.back().page_id > rids.front().page_id, "DELETE fixture did not span pages");

        auto deleted = engine.ExecuteSQL("DELETE FROM t WHERE active = TRUE");
        Check(deleted.type == PlanType::Delete && deleted.affected_rows == 6 && deleted.rows.empty() &&
              deleted.output_schema.GetColumnCount() == 0, "Multi-page DELETE result wrong");
        Check(engine.ExecuteSQL("SELECT * FROM t").rows.size() == 7, "DELETE skipped or duplicated rows");
        for (int i = 1; i < 12; i += 2) {
            Check(engine.ExecuteSQL("SELECT id FROM t WHERE id = " + std::to_string(i)).rows.size() == 1,
                  "DELETE removed nonmatching row");
        }
        auto& heap = catalog.GetTableHeap("t");
        const auto stable = Tuple::Deserialize(heap.GetRecord(rids[3]), catalog.GetTable("t").GetSchema());
        Check(stable.GetValue(0) == Value::Integer(3), "Other RID changed after page compaction");
        Check(heap.GetTupleMeta(rids[2]).is_deleted &&
                  Tuple::Deserialize(heap.GetRecord(rids[2]),
                                     catalog.GetTable("t").GetSchema()).GetValue(0) ==
                      Value::Integer(2),
              "DELETE did not preserve a logical tombstone at the stable RID");

        Check(engine.ExecuteSQL("DELETE FROM t WHERE id = 1").affected_rows == 1, "First live DELETE failed");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE id = 5").affected_rows == 1, "Middle live DELETE failed");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE id = 11").affected_rows == 1, "Last live DELETE failed");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE id = 999").affected_rows == 0, "No-match count wrong");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE FALSE").affected_rows == 0, "WHERE FALSE deleted rows");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE NULL").affected_rows == 0, "WHERE NULL deleted rows");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE id = NULL").affected_rows == 0, "NULL comparison deleted rows");
        Check(engine.ExecuteSQL("INSERT INTO t VALUES (99, 2147484999, TRUE, 'after')").affected_rows == 1,
              "INSERT after DELETE failed");
        Check(engine.ExecuteSQL("SELECT id FROM t").rows.size() == 5, "Post-delete rows wrong");

        engine.ExecuteSQL("INSERT INTO types VALUES (1, 2147483648, TRUE, 'a')");
        engine.ExecuteSQL("INSERT INTO types VALUES (2, 2147483649, FALSE, 'b')");
        engine.ExecuteSQL("INSERT INTO types VALUES (3, 2147483650, TRUE, 'c')");
        Check(engine.ExecuteSQL("DELETE FROM types WHERE big > 2147483649 AND active = TRUE").affected_rows == 1,
              "BIGINT/BOOLEAN/AND DELETE failed");
        Check(engine.ExecuteSQL("DELETE FROM types WHERE name = 'a' OR NOT active = TRUE").affected_rows == 2,
              "VARCHAR/OR/NOT DELETE failed");
        Check(engine.ExecuteSQL("SELECT * FROM types").rows.empty(), "Typed DELETE left rows");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto restored = engine.ExecuteSQL("SELECT id FROM t");
        Check(restored.rows.size() == 5 && engine.ExecuteSQL("SELECT id FROM t WHERE id = 2").rows.empty() &&
              engine.ExecuteSQL("SELECT id FROM t WHERE id = 99").rows.size() == 1,
              "DELETE persistence failed");
        Check(engine.ExecuteSQL("DELETE FROM t").affected_rows == 5, "DELETE all count wrong");
        Check(engine.ExecuteSQL("SELECT * FROM t").rows.empty(), "DELETE all left records");
        Check(engine.ExecuteSQL("DELETE FROM t").affected_rows == 0, "DELETE empty table count wrong");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        Check(engine.ExecuteSQL("SELECT * FROM t").rows.empty(), "DELETE all was not persisted");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-delete-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestParser();
        TestBindingAndPlanning(directory / "bind.udb");
        TestExecution(directory / "engine.udb");
        std::cout << "DELETE tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
