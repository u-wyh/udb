#include "udb/sql/executor.h"
#include "udb/sql/parser.h"
#include "udb/sql/binder.h"
#include "udb/sql/planner.h"
#include "udb/database.h"

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
ExecutionResult Run(Catalog& catalog, const std::string& sql) {
    const auto plan = Planner::Plan(Binder(catalog).Bind(Parser::Parse(sql)));
    return Executor(catalog).Execute(*plan);
}

void TestDirect(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    Executor executor(catalog);
    const Schema schema({Column("id", TypeId::INTEGER), Column("text", TypeId::VARCHAR, 20)});
    const auto created = executor.Execute(CreateTablePlan("t", schema));
    Check(created.type == PlanType::CreateTable && created.rows.empty() && created.affected_rows == 0 &&
          created.output_schema.GetColumnCount() == 0, "CREATE result wrong");
    const auto id = catalog.GetTable("t").GetTableId();
    const auto page_id = catalog.GetTable(id).GetFirstPageId();
    const SeqScanPlan scan(id, {1, 0}, Schema({schema.GetColumn(1), schema.GetColumn(0)}));
    Check(executor.Execute(scan).rows.empty(), "Empty SELECT not empty");
    const std::vector<Value> values{Value::Integer(7), Value::Varchar(std::string("a\0b", 3))};
    const auto inserted = executor.Execute(InsertPlan(id, schema, values));
    Check(inserted.type == PlanType::Insert && inserted.affected_rows == 1 && inserted.inserted_rid &&
          inserted.rows.empty() && inserted.output_schema.GetColumnCount() == 0, "INSERT result wrong");
    const auto tuple = Tuple::Deserialize(catalog.GetTableHeap(id).GetRecord(*inserted.inserted_rid), schema);
    Check(tuple.GetValue(0) == values[0] && tuple.GetValue(1) == values[1], "Stored INSERT wrong");
    auto* page = pool.FetchPage(page_id);
    const auto before = *page;
    pool.UnpinPage(page_id, false);
    const auto result = executor.Execute(scan);
    Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == values[1] && result.rows[0].GetValue(1) == values[0] &&
          result.output_schema.GetColumn(0).GetName() == "text" && result.affected_rows == 0, "Projection wrong");
    page = pool.FetchPage(page_id);
    Check(page->data == before.data, "SELECT changed page");
    pool.UnpinPage(page_id, false);
    Reject<std::logic_error>([&] { pool.UnpinPage(page_id, false); });
    Check(disk.ReadPage(page_id).data == Page{}.data, "Executor unexpectedly flushed");
    Reject<std::invalid_argument>([&] { executor.Execute(CreateTablePlan("t", schema)); });
    Reject<std::out_of_range>([&] { executor.Execute(InsertPlan(99, schema, values)); });
    Reject<std::invalid_argument>([&] { executor.Execute(InsertPlan(id, Schema({}), {})); });
    Reject<std::invalid_argument>([&] { executor.Execute(InsertPlan(id, schema, {Value::Boolean(true), values[1]})); });
    Reject<std::invalid_argument>([&] { executor.Execute(SeqScanPlan(id, {99}, Schema({schema.GetColumn(0)}))); });
    Reject<std::invalid_argument>([&] { executor.Execute(SeqScanPlan(id, {0}, Schema({}))); });
    Reject<std::invalid_argument>([&] { executor.Execute(SeqScanPlan(id, {0}, Schema({schema.GetColumn(1)}))); });
    Check(executor.Execute(scan).rows.size() == 1, "Failed execution changed rows");
    // A corrupt Record must fail deserialization without leaking a pin.
    const auto bad = catalog.GetTableHeap(id).InsertRecord(Record("x", 1));
    Reject<std::runtime_error>([&] { executor.Execute(scan); });
    pool.FetchPage(page_id);
    pool.UnpinPage(page_id, false);
    Reject<std::logic_error>([&] { pool.UnpinPage(page_id, false); });
    catalog.GetTableHeap(id).DeleteRecord(bad);  // Fixture cleanup, no SQL DELETE support.
    Check(executor.Execute(scan).rows.size() == 1, "Scan did not skip deleted slot");
}

void TestPipeline(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        Run(catalog, "CREATE TABLE users (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(2000))");
        Run(catalog, "CREATE TABLE other (id INTEGER)");
        Check(Run(catalog, "SELECT * FROM users").rows.empty(), "New SQL table not empty");
        Run(catalog, "INSERT INTO other VALUES (42)");
        Run(catalog, "INSERT INTO users VALUES (NULL, NULL, NULL, NULL)");
        Run(catalog, u8"INSERT INTO users VALUES (-1, -2147483649, FALSE, '你好')");
        for (int i = 0; i < 12; ++i) {
            const auto inserted = Run(catalog, "INSERT INTO users VALUES (" + std::to_string(i) +
                ", 2147483648, TRUE, '" + std::string(1400, static_cast<char>('a' + i)) + "')");
            Check(inserted.affected_rows == 1 && inserted.inserted_rid.has_value(), "SQL INSERT failed");
        }
        const auto all = Run(catalog, "SELECT * FROM users");
        Check(all.rows.size() == 14 && all.output_schema.GetColumnCount() == 4, "Multi-page scan lost rows");
        for (int i = 0; i < 12; ++i) {
            Check(all.rows[i + 2].GetValue(0) == Value::Integer(i) && all.rows[i + 2].GetValue(1) == Value::BigInt(2147483648LL) &&
                  all.rows[i + 2].GetValue(2).GetBoolean() && all.rows[i + 2].GetValue(3).GetVarchar() == std::string(1400, static_cast<char>('a' + i)),
                  "SQL data/order mismatch");
        }
        for (std::size_t i = 0; i < 4; ++i) {
            Check(all.rows[0].GetValue(i).IsNull() && all.rows[0].GetValue(i).GetType() == all.output_schema.GetColumn(i).GetType(), "NULL lost");
        }
        Check(all.rows[1].GetValue(3).GetVarchar() == u8"你好" && !all.rows[1].GetValue(2).GetBoolean(), "UTF8/BOOLEAN wrong");
        const auto one = Run(catalog, "SELECT name FROM users");
        const auto reordered = Run(catalog, "SELECT name, id FROM users");
        for (std::size_t i = 0; i < all.rows.size(); ++i) {
            Check(one.rows[i].GetValue(0) == all.rows[i].GetValue(3) && reordered.rows[i].GetValue(0) == all.rows[i].GetValue(3) &&
                  reordered.rows[i].GetValue(1) == all.rows[i].GetValue(0), "SELECT column order wrong");
        }
        Check(Run(catalog, "SELECT * FROM other").rows[0].GetValue(0) == Value::Integer(42), "Tables interfered");
        database->Flush();
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        const auto restored = Run(database->GetCatalog(), "SELECT name, id FROM users");
        Check(restored.rows.size() == 14 && restored.output_schema.GetColumn(0).GetName() == "name" &&
              restored.output_schema.GetColumn(1).GetName() == "id", "Reopen schema/order wrong");
        for (int i = 0; i < 12; ++i) {
            Check(restored.rows[i + 2].GetValue(0).GetVarchar() == std::string(1400, static_cast<char>('a' + i)) &&
                  restored.rows[i + 2].GetValue(1) == Value::Integer(i), "Reopen data wrong");
        }
        Check(restored.rows[0].GetValue(0).IsNull() && restored.rows[1].GetValue(0).GetVarchar() == u8"你好", "Reopen NULL/UTF8 wrong");
        database->Close();
    }
}
}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-executor-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestDirect(directory / "direct.udb");
        TestPipeline(directory / "pipeline.udb");
        std::cout << "Executor tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
