#include "udb/database.h"
#include "udb/slotted_page.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <cstring>
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

Record Bytes(std::size_t size, char value) {
    return Record(std::string(size, value).data(), size);
}

bool Equal(const Record& left, const Record& right) {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || std::memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

void TestStorage(const std::filesystem::path& path) {
    Page page;
    SlottedPage view(page, 7);
    view.Init();
    const auto first = view.InsertRecord(Bytes(100, 'a')).value();
    const auto middle = view.InsertRecord(Bytes(120, 'b')).value();
    const auto last = view.InsertRecord(Bytes(80, 'c')).value();
    Check(view.UpdateRecord(middle, Bytes(120, 's')) && Equal(view.GetRecord(middle), Bytes(120, 's')),
          "Same-size update failed");
    Check(view.UpdateRecord(first, Bytes(20, 'x')) && Equal(view.GetRecord(first), Bytes(20, 'x')),
          "Shorter update failed");
    Check(view.UpdateRecord(last, Bytes(300, 'z')) && Equal(view.GetRecord(last), Bytes(300, 'z')),
          "Longer update failed");
    Check(first == RID{7, 0} && middle == RID{7, 1} && last == RID{7, 2} &&
          Equal(view.GetRecord(first), Bytes(20, 'x')) && Equal(view.GetRecord(middle), Bytes(120, 's')),
          "Update changed another RID or record");
    const auto deleted = view.InsertRecord(Bytes(1, 'd')).value();
    view.DeleteRecord(deleted);
    Reject<std::out_of_range>([&] { view.UpdateRecord(deleted, Bytes(1, 'q')); });
    Reject<std::out_of_range>([&] { view.UpdateRecord({8, 0}, Bytes(1, 'q')); });

    view.Init();
    const auto constrained = view.InsertRecord(Bytes(100, 'o')).value();
    view.InsertRecord(Bytes(view.GetFreeSpace(), 'f')).value();
    const auto before = page;
    Check(!view.UpdateRecord(constrained, Bytes(101, 'n')) && page.data == before.data &&
          Equal(view.GetRecord(constrained), Bytes(100, 'o')), "Failed update changed page");

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    TableHeap table(pool);
    const auto rid = table.InsertRecord(Bytes(100, 'a'));
    const auto filler = table.InsertRecord(
        Bytes(PAGE_SIZE - SlottedPage::HEADER_SIZE - 2 * SlottedPage::SLOT_SIZE - 100, 'f'));
    const auto old = table.GetRecord(rid);
    Check(!table.UpdateRecord(rid, Bytes(101, 'b')) && Equal(table.GetRecord(rid), old),
          "TableHeap failed update changed record");
    Reject<std::out_of_range>([&] { table.UpdateRecord({rid.page_id, 999}, Bytes(1, 'x')); });
    table.DeleteRecord(filler);
    Reject<std::out_of_range>([&] { table.UpdateRecord(filler, Bytes(1, 'x')); });
}

void TestParser() {
    const auto single = std::get<UpdateStatement>(Parser::Parse("UPDATE users SET name = 'Bob';"));
    Check(single.table_name == "users" && single.assignments.size() == 1 &&
          single.assignments[0].column_name == "name" &&
          std::get<std::string>(single.assignments[0].value) == "Bob" && !single.predicate,
          "Single UPDATE AST wrong");
    const auto multiple = std::get<UpdateStatement>(Parser::Parse(
        "UPDATE users SET name = 'Bob', active = TRUE WHERE id >= 10"));
    Check(multiple.assignments.size() == 2 && multiple.predicate &&
          std::holds_alternative<ComparisonExpression>(multiple.predicate->node), "Multi UPDATE AST wrong");
    for (const auto sql : {"UPDATE", "UPDATE t", "UPDATE SET id = 1", "UPDATE t SET",
                           "UPDATE t SET id", "UPDATE t SET id =", "UPDATE t SET id = other",
                           "UPDATE t SET id = 1,", "UPDATE t SET id = 1 extra", "UPDATE t SET id = 1;;"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }
}

void TestBindingAndPlanning(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER), Column("big", TypeId::BIGINT),
                         Column("active", TypeId::BOOLEAN), Column("name", TypeId::VARCHAR, 5)});
    const auto& table = catalog.CreateTable("t", schema);
    const Binder binder(catalog);
    const auto bound = std::get<BoundUpdateStatement>(binder.Bind(Parser::Parse(
        "UPDATE t SET name = 'Bob', active = TRUE, big = 1 WHERE id = 1")));
    Check(bound.table_id == table.GetTableId() && bound.assignments.size() == 3 &&
          bound.assignments[0].column_index == 3 && bound.assignments[0].value == Value::Varchar("Bob") &&
          bound.assignments[1].column_index == 2 && bound.assignments[1].value == Value::Boolean(true) &&
          bound.assignments[2].value == Value::BigInt(1) && bound.predicate,
          "UPDATE binding wrong");
    const auto null = std::get<BoundUpdateStatement>(binder.Bind(Parser::Parse("UPDATE t SET name = NULL")));
    Check(null.assignments[0].value == Value::Null(TypeId::VARCHAR), "UPDATE NULL typing wrong");
    for (const auto sql : {"UPDATE missing SET id = 1", "UPDATE t SET missing = 1",
                           "UPDATE t SET id = 1, id = 2", "UPDATE t SET id = TRUE",
                           "UPDATE t SET name = '123456'",
                           "UPDATE t SET id = 1 WHERE missing = 1", "UPDATE t SET id = 1 WHERE name"}) {
        Reject<BindError>([&] { binder.Bind(Parser::Parse(sql)); });
    }
    const auto plan_node = Planner::Plan(BoundStatement{bound});
    const auto& plan = dynamic_cast<const UpdatePlan&>(*plan_node);
    Check(plan.GetType() == PlanType::Update && plan.GetTableId() == table.GetTableId() &&
          plan.GetAssignments().size() == 3 && plan.GetPredicate() &&
          plan.GetOutputSchema().GetColumnCount() == 0, "UPDATE plan wrong");
    Check(!catalog.GetTableHeap("t").GetFirstRID(), "Binder or Planner executed UPDATE");
}

void TestExecution(const std::filesystem::path& path) {
    std::vector<RID> rids;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(2000))");
        for (int i = 0; i < 12; ++i) {
            const auto inserted = engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) +
                ", 2147484000, " + (i % 2 == 0 ? std::string("TRUE") : std::string("FALSE")) +
                ", '" + std::string(1400, static_cast<char>('a' + i)) + "')");
            rids.push_back(*inserted.inserted_rid);
        }
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, NULL, NULL)");
        Check(rids.back().page_id > rids.front().page_id, "UPDATE fixture did not span pages");

        Check(engine.ExecuteSQL("UPDATE t SET name = 'Bob' WHERE id = 1").affected_rows == 1,
              "Single-column UPDATE failed");
        Check(engine.ExecuteSQL("UPDATE t SET big = 9223372036854775807, active = TRUE WHERE id = 1").affected_rows == 1,
              "Multi-column UPDATE failed");
        auto row = engine.ExecuteSQL("SELECT * FROM t WHERE id = 1").rows.at(0);
        Check(row.GetValue(1) == Value::BigInt(INT64_MAX) && row.GetValue(2) == Value::Boolean(true) &&
              row.GetValue(3) == Value::Varchar("Bob"), "Updated values not visible");
        const auto& schema = catalog.GetTable("t").GetSchema();
        Check(Tuple::Deserialize(catalog.GetTableHeap("t").GetRecord(rids[1]), schema).GetValue(3) == Value::Varchar("Bob"),
              "UPDATE changed RID");

        Check(engine.ExecuteSQL("UPDATE t SET active = FALSE").affected_rows == 13, "UPDATE all count wrong");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE active = TRUE").rows.empty(), "UPDATE all missed rows");
        Check(engine.ExecuteSQL("UPDATE t SET id = 99 WHERE id = 0").affected_rows == 1, "INTEGER UPDATE failed");
        Check(engine.ExecuteSQL("UPDATE t SET name = NULL WHERE id = 2").affected_rows == 1, "NULL UPDATE failed");
        Check(engine.ExecuteSQL("SELECT name FROM t WHERE id = 2").rows.at(0).GetValue(0).IsNull(), "NULL not stored");
        Check(engine.ExecuteSQL("UPDATE t SET name = 'none' WHERE id = 999").affected_rows == 0,
              "No-match UPDATE count wrong");
        Check(engine.ExecuteSQL("UPDATE t SET name = 'none' WHERE NULL").affected_rows == 0,
              "WHERE NULL updated rows");
        Check(engine.ExecuteSQL("UPDATE t SET name = 'multi' WHERE id >= 5 AND NOT id = 9").affected_rows == 7,
              "Multi-page predicate UPDATE count wrong");
        Check(engine.ExecuteSQL("SELECT name FROM t WHERE id = 9").rows.at(0).GetValue(0) != Value::Varchar("multi"),
              "Unmatched row changed");
        Check(Tuple::Deserialize(catalog.GetTableHeap("t").GetRecord(rids[3]), schema).GetValue(0) == Value::Integer(3),
              "Other RID changed");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        Check(engine.ExecuteSQL("SELECT name FROM t WHERE id = 1").rows.at(0).GetValue(0) == Value::Varchar("Bob") &&
              engine.ExecuteSQL("SELECT id FROM t WHERE id = 99").rows.size() == 1 &&
              engine.ExecuteSQL("SELECT name FROM t WHERE id = 2").rows.at(0).GetValue(0).IsNull(),
              "UPDATE persistence failed");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-update-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestStorage(directory / "storage.udb");
        TestParser();
        TestBindingAndPlanning(directory / "bind.udb");
        TestExecution(directory / "engine.udb");
        std::cout << "UPDATE tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
