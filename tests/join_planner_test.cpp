#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/executor.h"
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

void Test(Catalog& catalog) {
    const Schema schema({Column("i", TypeId::INTEGER), Column("b", TypeId::BIGINT),
        Column("flag", TypeId::BOOLEAN), Column("text", TypeId::VARCHAR, 20), Column("d", TypeId::DOUBLE)});
    const auto left = catalog.CreateTable("l", schema).GetTableId();
    const auto right = catalog.CreateTable("r", schema).GetTableId();
    const Tuple row(schema, {Value::Integer(1), Value::BigInt(5000000000), Value::Boolean(true),
        Value::Varchar(std::string("a\0b", 3)), Value::Double(1.5)});
    catalog.GetTableHeap(left).InsertRecord(row.Serialize(schema));
    catalog.GetTableHeap(right).InsertRecord(row.Serialize(schema));
    catalog.GetTableHeap(right).InsertRecord(row.Serialize(schema));
    const auto before = catalog.ListTables();
    for (const auto* column : {"i", "b", "flag", "text", "d"}) {
        for (const bool reversed : {false, true}) {
            const auto a = std::string(reversed ? "r." : "l.") + column;
            const auto b = std::string(reversed ? "l." : "r.") + column;
            const auto bound = Binder(catalog).Bind(Parser::Parse(
                "SELECT r.text, l.i FROM l JOIN r ON " + a + " = " + b +
                " WHERE l.i = 1 ORDER BY l.i DESC LIMIT 1 OFFSET 1"));
            const auto optimized = Planner::Plan(bound, catalog);
            const auto reference = Planner::Plan(bound);
            Check(optimized->GetType() == (std::string(column) == "d" ?
                PlanType::NestedLoopJoin : PlanType::HashJoin), "Wrong join choice");
            Check(reference->GetType() == PlanType::NestedLoopJoin, "Reference must use nested loop");
            const auto& plan = dynamic_cast<const JoinPlan&>(*optimized);
            Check(plan.GetLeftTableId() == left && plan.GetRightTableId() == right &&
                  plan.GetColumnIndexes() == std::vector<std::size_t>({8, 0}) &&
                  plan.GetPredicate() && plan.GetLimit() == 1 && plan.GetOffset() == 1,
                  "Planner lost bound information");
            Executor executor(catalog);
            const auto actual = executor.Execute(*optimized);
            const auto expected = executor.Execute(*reference);
            Check(actual.rows.size() == 1 && expected.rows.size() == 1 &&
                  actual.rows[0].GetValue(0) == expected.rows[0].GetValue(0) &&
                  actual.rows[0].GetValue(1) == expected.rows[0].GetValue(1), "Planner changed results");
        }
    }
    const auto cross = Binder(catalog).Bind(Parser::Parse("SELECT * FROM l CROSS JOIN r"));
    Check(Planner::Plan(cross, catalog)->GetType() == PlanType::CrossJoin, "CROSS JOIN used hashing");
    const auto single = Binder(catalog).Bind(Parser::Parse("SELECT * FROM l"));
    Check(Planner::Plan(single, catalog)->GetType() == PlanType::SeqScan, "Single scan changed");
    Check(before == catalog.ListTables(), "Planner mutated catalog");
}
}  // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-join-planner-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 1);
        Test(database->GetCatalog());
        database->Close();
        std::cout << "Join planner tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
