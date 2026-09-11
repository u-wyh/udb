#include "udb/sql/planner.h"
#include "udb/sql/binder.h"
#include "udb/sql/parser.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void CheckSchema(const Schema& actual, const Schema& expected) {
    Check(actual.GetColumnCount() == expected.GetColumnCount(), "Schema count mismatch");
    for (std::size_t i = 0; i < actual.GetColumnCount(); ++i) {
        const auto& a = actual.GetColumn(i);
        const auto& b = expected.GetColumn(i);
        Check(a.GetName() == b.GetName() && a.GetType() == b.GetType() && a.GetMaxLength() == b.GetMaxLength(),
              "Schema column mismatch");
    }
}

void TestOwnedPlans() {
    const Schema schema({Column("id", TypeId::INTEGER), Column("name", TypeId::VARCHAR, 8)});
    BoundStatement bound = BoundCreateTableStatement{"users", schema};
    const auto create_node = Planner::Plan(bound);
    const auto& create = dynamic_cast<const CreateTablePlan&>(*create_node);
    Check(create.GetType() == PlanType::CreateTable && create.GetTableName() == "users", "CREATE plan identity wrong");
    CheckSchema(create.GetTableSchema(), schema);
    Check(create.GetOutputSchema().GetColumnCount() == 0 && create.GetChildren().empty(), "CREATE output/children wrong");
    std::get<BoundCreateTableStatement>(bound).table_name = "changed";
    Check(create.GetTableName() == "users", "Plan aliases bound name");
    const std::vector<Value> values{Value::Integer(7), Value::Varchar(std::string("a\0b", 3))};
    bound = BoundInsertStatement{42, "users", schema, values};
    const auto insert_node = Planner::Plan(bound);
    const auto& insert = dynamic_cast<const InsertPlan&>(*insert_node);
    Check(insert.GetType() == PlanType::Insert && insert.GetTableId() == 42 && insert.GetValues() == values, "INSERT payload wrong");
    CheckSchema(insert.GetTableSchema(), schema);
    Check(insert.GetOutputSchema().GetColumnCount() == 0 && insert.GetChildren().empty(), "INSERT output/children wrong");
    std::get<BoundInsertStatement>(bound).values[0] = Value::Integer(9);
    Check(insert.GetValues()[0] == Value::Integer(7), "Plan aliases bound values");
    bound = BoundSelectStatement{42, "users", {1, 0},
        Schema({schema.GetColumn(1), schema.GetColumn(0)}), nullptr, {}, std::nullopt, 0, {}};
    const auto scan_node = Planner::Plan(bound);
    const auto& scan = dynamic_cast<const SeqScanPlan&>(*scan_node);
    Check(scan.GetType() == PlanType::SeqScan && scan.GetTableId() == 42 &&
          scan.GetColumnIndexes() == std::vector<std::size_t>({1, 0}) && scan.GetChildren().empty(), "Scan mapping wrong");
    CheckSchema(scan.GetOutputSchema(), Schema({schema.GetColumn(1), schema.GetColumn(0)}));
    std::get<BoundSelectStatement>(bound).column_indexes.clear();
    std::get<BoundSelectStatement>(bound).output_schema = Schema({});
    Check(scan.GetColumnIndexes().size() == 2 && scan.GetOutputSchema().GetColumnCount() == 2, "Plan aliases bound projection");
}

void TestPipeline(const std::filesystem::path& path) {
    std::vector<BoundStatement> bound_statements;
    table_id_t table_id;
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        Catalog catalog(pool);
        const Binder binder(catalog);
        const auto create = binder.Bind(Parser::Parse(
            "CREATE TABLE users (id INTEGER, name VARCHAR(40), active BOOLEAN, big BIGINT)"));
        const auto create_node = Planner::Plan(create);
        Check(catalog.ListTables().empty() && std::filesystem::file_size(path) == 0, "Planner executed CREATE");
        catalog.CreateTable("other", Schema({}));
        const auto& create_bound = std::get<BoundCreateTableStatement>(create);
        const auto& table = catalog.CreateTable(create_bound.table_name, create_bound.schema);
        table_id = table.GetTableId();
        Check(table_id != 0, "Fixture should exercise a nonzero table ID");
        // Planning must not recheck whether CREATE's table name now exists.
        Check(Planner::Plan(create)->GetType() == PlanType::CreateTable, "Planner repeated CREATE binding");
        const auto tables = catalog.ListTables();
        const auto size = std::filesystem::file_size(path);
        auto* page = pool.FetchPage(table.GetFirstPageId());
        const auto saved = *page;
        page->data[0] = 0;  // A planner must not inspect table page contents.
        for (const auto sql : {"INSERT INTO users VALUES (1, 'Alice', TRUE, 2147483648)",
             "INSERT INTO users VALUES (NULL, NULL, NULL, NULL)", "SELECT * FROM users",
             "SELECT name FROM users", "SELECT name, id FROM users"}) {
            bound_statements.push_back(binder.Bind(Parser::Parse(sql)));
            const auto plan = Planner::Plan(bound_statements.back());
            Check(plan->GetChildren().empty(), "Current plans must be leaves");
        }
        Check(catalog.ListTables() == tables && std::filesystem::file_size(path) == size && page->data[0] == 0,
              "Planner modified catalog/data");
        *page = saved;
        pool.UnpinPage(table.GetFirstPageId(), false);
        Check(!catalog.GetTableHeap(table_id).GetFirstRID(), "Planner executed INSERT");
    }
    // Bound snapshots can be planned after all Catalog/BufferPool objects die.
    const auto insert_node = Planner::Plan(bound_statements[0]);
    const auto& insert = dynamic_cast<const InsertPlan&>(*insert_node);
    Check(insert.GetTableId() == table_id && insert.GetValues() == std::vector<Value>({Value::Integer(1),
          Value::Varchar("Alice"), Value::Boolean(true), Value::BigInt(2147483648LL)}), "Pipeline INSERT values wrong");
    const auto null_node = Planner::Plan(bound_statements[1]);
    const auto& null_insert = dynamic_cast<const InsertPlan&>(*null_node);
    for (std::size_t i = 0; i < 4; ++i) {
        Check(null_insert.GetValues()[i].IsNull() && null_insert.GetValues()[i].GetType() == null_insert.GetTableSchema().GetColumn(i).GetType(),
              "Typed NULL lost in plan");
    }
    const std::vector<std::vector<std::size_t>> expected{{0, 1, 2, 3}, {1}, {1, 0}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto node = Planner::Plan(bound_statements[i + 2]);
        const auto& scan = dynamic_cast<const SeqScanPlan&>(*node);
        Check(scan.GetTableId() == table_id && scan.GetColumnIndexes() == expected[i], "Pipeline projection indexes wrong");
        std::vector<Column> columns;
        for (const auto index : expected[i]) { columns.push_back(insert.GetTableSchema().GetColumn(index)); }
        CheckSchema(scan.GetOutputSchema(), Schema(std::move(columns)));
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-planner-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestOwnedPlans();
        TestPipeline(directory / "planner.udb");
        std::cout << "Planner tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
