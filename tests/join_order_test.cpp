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

void Fill(Catalog& catalog, const Schema& schema, const std::string& table, int count) {
    for (int i = 0; i < count; ++i) {
        Tuple tuple(schema, {Value::Integer(i), Value::Double(static_cast<double>(i)),
                             Value::Varchar(table)});
        catalog.GetTableHeap(table).InsertRecord(tuple.Serialize(schema));
    }
}

void TestJoinOrder(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    auto& catalog = database->GetCatalog();
    SqlEngine sql(catalog);
    const Schema schema({Column("id", TypeId::INTEGER), Column("d", TypeId::DOUBLE),
                         Column("label", TypeId::VARCHAR, 20)});
    for (const auto* name : {"large_left", "small_right", "small_left", "large_right"}) {
        catalog.CreateTable(name, schema);
    }
    Fill(catalog, schema, "large_left", 40);
    Fill(catalog, schema, "small_right", 3);
    Fill(catalog, schema, "small_left", 2);
    Fill(catalog, schema, "large_right", 30);

    const auto unanalysed_hash = Plan(catalog,
        "SELECT large_left.id FROM large_left JOIN small_right "
        "ON large_left.id = small_right.id");
    Check(unanalysed_hash->GetType() == PlanType::HashJoin &&
          !dynamic_cast<const JoinPlan&>(*unanalysed_hash).IsSmallerInputLeft(),
          "Missing statistics changed the established hash build side");

    for (const auto* name : {"large_left", "small_right", "small_left", "large_right"}) {
        catalog.AnalyzeTable(name);
    }
    const auto hash_right_small = Plan(catalog,
        "SELECT large_left.id, small_right.label FROM large_left JOIN small_right "
        "ON large_left.id = small_right.id ORDER BY large_left.id");
    Check(hash_right_small->GetType() == PlanType::HashJoin &&
          !dynamic_cast<const JoinPlan&>(*hash_right_small).IsSmallerInputLeft(),
          "Hash Join did not build the smaller right input");
    const auto hash_left_small = Plan(catalog,
        "SELECT small_left.id, large_right.label FROM small_left JOIN large_right "
        "ON small_left.id = large_right.id ORDER BY small_left.id");
    Check(hash_left_small->GetType() == PlanType::HashJoin &&
          dynamic_cast<const JoinPlan&>(*hash_left_small).IsSmallerInputLeft(),
          "Hash Join did not build the smaller left input");
    const auto nested_right_small = Plan(catalog,
        "SELECT large_left.id FROM large_left JOIN small_right "
        "ON large_left.d = small_right.d ORDER BY large_left.id");
    Check(nested_right_small->GetType() == PlanType::NestedLoopJoin &&
          !dynamic_cast<const JoinPlan&>(*nested_right_small).IsSmallerInputLeft(),
          "Nested loop did not use the smaller right outer input");
    const auto nested_left_small = Plan(catalog,
        "SELECT small_left.id FROM small_left JOIN large_right "
        "ON small_left.d = large_right.d ORDER BY small_left.id");
    Check(nested_left_small->GetType() == PlanType::NestedLoopJoin &&
          dynamic_cast<const JoinPlan&>(*nested_left_small).IsSmallerInputLeft(),
          "Nested loop did not use the smaller left outer input");

    const auto hash_result = sql.ExecuteSQL(
        "SELECT large_left.id, small_right.label FROM large_left JOIN small_right "
        "ON large_left.id = small_right.id ORDER BY large_left.id");
    Check(hash_result.rows.size() == 3 && hash_result.rows[0].GetValue(0) == Value::Integer(0) &&
          hash_result.rows[2].GetValue(0) == Value::Integer(2) &&
          hash_result.rows[1].GetValue(1) == Value::Varchar("small_right"),
          "Hash physical reordering changed logical output");
    const auto nested_result = sql.ExecuteSQL(
        "SELECT large_left.label, small_right.id FROM large_left JOIN small_right "
        "ON large_left.d = small_right.d ORDER BY small_right.id DESC");
    Check(nested_result.rows.size() == 3 &&
          nested_result.rows[0].GetValue(0) == Value::Varchar("large_left") &&
          nested_result.rows[0].GetValue(1) == Value::Integer(2),
          "Nested-loop physical reordering changed projection or ordering");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-join-order-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestJoinOrder(directory / "join-order.udb");
        std::cout << "Join order tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
