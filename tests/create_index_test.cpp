#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error = std::exception, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(static_cast<bool>(output), "Cannot write metadata fixture");
}

void Put(std::string& bytes, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

RID Insert(Catalog& catalog, table_id_t table_id, std::vector<Value> values) {
    const auto& schema = catalog.GetTable(table_id).GetSchema();
    return catalog.GetTableHeap(table_id).InsertRecord(Tuple(schema, std::move(values)).Serialize(schema));
}

Tuple IndexedTuple(const Catalog& catalog, const std::string& index_name, std::int64_t key) {
    const auto& index = catalog.GetIndex(index_name);
    const auto rid = index.GetTree().GetValue(key);
    Check(rid.has_value(), "Index lookup missed an existing key");
    const auto& metadata = index.GetMetadata();
    const auto& schema = catalog.GetTable(metadata.GetTableId()).GetSchema();
    return Tuple::Deserialize(catalog.GetTableHeap(metadata.GetTableId()).GetRecord(*rid), schema);
}

void TestParserBinderPlanner(const std::filesystem::path& path) {
    const auto ast = std::get<CreateIndexStatement>(
        Parser::Parse("CrEaTe InDeX idx_users_id On users(id);"));
    Check(ast.index_name == "idx_users_id" && ast.table_name == "users" &&
          ast.column_name == "id", "CREATE INDEX AST is wrong");
    for (const auto sql : {"CREATE INDEX", "CREATE INDEX idx", "CREATE INDEX idx users(id)",
                           "CREATE INDEX idx ON users", "CREATE INDEX idx ON users()",
                           "CREATE INDEX idx ON users(id,)", "CREATE UNIQUE INDEX idx ON users(id)",
                           "CREATE INDEX idx ON users(id) extra"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    DiskManager disk(path);
    BufferPoolManager pool(disk, 2);
    Catalog catalog(pool);
    const auto& users = catalog.CreateTable("users", Schema({
        Column("id", TypeId::INTEGER), Column("big", TypeId::BIGINT),
        Column("name", TypeId::VARCHAR, 20), Column("flag", TypeId::BOOLEAN)}));
    const Binder binder(catalog);
    const auto bound = std::get<BoundCreateIndexStatement>(binder.Bind(Statement{ast}));
    Check(bound.index_name == "idx_users_id" && bound.table_id == users.GetTableId() &&
          bound.column_index == 0, "CREATE INDEX binding is wrong");
    const auto plan_node = Planner::Plan(BoundStatement{bound});
    const auto& plan = dynamic_cast<const CreateIndexPlan&>(*plan_node);
    Check(plan.GetType() == PlanType::CreateIndex && plan.GetIndexName() == "idx_users_id" &&
          plan.GetTableId() == users.GetTableId() && plan.GetColumnIndex() == 0 &&
          catalog.ListIndexes().empty(), "CREATE INDEX planning changed catalog or lost binding");
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX x ON missing(id)")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX x ON users(missing)")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX x ON users(flag)")); });

    catalog.CreateIndex("idx_users_id", users.GetTableId(), 0, BPlusTreeOptions{3, 3});
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX idx_users_id ON users(big)")); });
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX another ON users(id)")); });
    const auto& other = catalog.CreateTable("other", Schema({Column("id", TypeId::INTEGER)}));
    Reject<BindError>([&] { binder.Bind(Parser::Parse("CREATE INDEX idx_users_id ON other(id)")); });
    Check(catalog.GetTableIndexes(users.GetTableId()) == std::vector<index_id_t>{0} &&
          catalog.GetTableIndexes(other.GetTableId()).empty(), "Catalog table index listing is wrong");
}

void TestSqlBuildAndPersistence(const std::filesystem::path& path) {
    index_id_t integer_index_id;
    index_id_t bigint_index_id;
    index_id_t large_index_id;
    page_id_t integer_header;
    page_id_t large_header;
    table_id_t numbers_id;
    table_id_t large_id;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE numbers (i INTEGER, b BIGINT, note VARCHAR(10))");
        numbers_id = catalog.GetTable("numbers").GetTableId();
        const std::vector<std::vector<Value>> rows = {
            {Value::Integer(std::numeric_limits<std::int32_t>::min()), Value::BigInt(std::numeric_limits<std::int64_t>::min()), Value::Varchar("min")},
            {Value::Integer(-7), Value::BigInt(-2147483649LL), Value::Varchar("negative")},
            {Value::Integer(0), Value::BigInt(2147483648LL), Value::Varchar("zero")},
            {Value::Integer(std::numeric_limits<std::int32_t>::max()), Value::BigInt(std::numeric_limits<std::int64_t>::max()), Value::Varchar("max")},
            {Value::Null(TypeId::INTEGER), Value::Null(TypeId::BIGINT), Value::Varchar("null")}};
        for (const auto& values : rows) { Insert(catalog, numbers_id, values); }

        const auto integer_result = engine.ExecuteSQL("CREATE INDEX idx_i ON numbers(i)");
        const auto bigint_result = engine.ExecuteSQL("CREATE INDEX idx_b ON numbers(b)");
        Check(integer_result.type == PlanType::CreateIndex && bigint_result.type == PlanType::CreateIndex,
              "SQL CREATE INDEX result type is wrong");
        const auto& integer_index = catalog.GetIndex("idx_i");
        const auto& bigint_index = catalog.GetIndex("idx_b");
        integer_index_id = integer_index.GetMetadata().GetIndexId();
        bigint_index_id = bigint_index.GetMetadata().GetIndexId();
        integer_header = integer_index.GetMetadata().GetHeaderPageId();
        Check(integer_index.GetMetadata().GetTableId() == numbers_id &&
              integer_index.GetMetadata().GetColumnIndex() == 0 &&
              IndexedTuple(catalog, "idx_i", std::numeric_limits<std::int32_t>::min()).GetValue(2) == Value::Varchar("min") &&
              IndexedTuple(catalog, "idx_i", -7).GetValue(2) == Value::Varchar("negative") &&
              IndexedTuple(catalog, "idx_i", 0).GetValue(2) == Value::Varchar("zero") &&
              IndexedTuple(catalog, "idx_i", std::numeric_limits<std::int32_t>::max()).GetValue(2) == Value::Varchar("max") &&
              !integer_index.GetTree().GetValue(123), "INTEGER index lookup or metadata is wrong");
        Check(IndexedTuple(catalog, "idx_b", std::numeric_limits<std::int64_t>::min()).GetValue(2) == Value::Varchar("min") &&
              IndexedTuple(catalog, "idx_b", std::numeric_limits<std::int64_t>::max()).GetValue(2) == Value::Varchar("max"),
              "BIGINT boundary index lookup failed");
        Reject<BindError>([&] { engine.ExecuteSQL("CREATE INDEX idx_i ON numbers(b)"); });
        Reject<BindError>([&] { engine.ExecuteSQL("CREATE INDEX idx_i2 ON numbers(i)"); });
        engine.ExecuteSQL("CREATE TABLE flags (flag BOOLEAN)");
        Reject<BindError>([&] { engine.ExecuteSQL("CREATE INDEX idx_flag ON flags(flag)"); });

        engine.ExecuteSQL("CREATE TABLE duplicates (id INTEGER)");
        engine.ExecuteSQL("INSERT INTO duplicates VALUES (1)");
        engine.ExecuteSQL("INSERT INTO duplicates VALUES (2)");
        engine.ExecuteSQL("INSERT INTO duplicates VALUES (1)");
        const auto before_failed_index = catalog.ListIndexes();
        Reject<std::invalid_argument>([&] { engine.ExecuteSQL("CREATE INDEX idx_duplicate ON duplicates(id)"); });
        Check(catalog.ListIndexes() == before_failed_index &&
              engine.ExecuteSQL("SELECT * FROM duplicates").rows.size() == 3,
              "Failed unique index build published metadata or damaged table data");

        engine.ExecuteSQL("CREATE TABLE large_table (id INTEGER)");
        large_id = catalog.GetTable("large_table").GetTableId();
        for (int i = 0; i < 1200; ++i) { Insert(catalog, large_id, {Value::Integer(i - 600)}); }
        const auto& large_index = catalog.CreateIndex("idx_large", large_id, 0, BPlusTreeOptions{8, 4});
        large_index_id = large_index.GetMetadata().GetIndexId();
        large_header = large_index.GetMetadata().GetHeaderPageId();
        Check(large_index.GetTree().GetHeight() >= 3 && large_header != large_index.GetTree().GetRootPageId(),
              "Large index did not split through an internal/root level");
        large_index.GetTree().Validate();
        for (int key = -600; key < 600; ++key) {
            Check(large_index.GetTree().GetValue(key).has_value(), "Large index lookup failed");
        }

        engine.ExecuteSQL("CREATE TABLE empty_table (id INTEGER)");
        engine.ExecuteSQL("CREATE INDEX idx_empty ON empty_table(id)");
        Check(catalog.GetIndex("idx_empty").GetMetadata().GetIndexId() == large_index_id + 1 &&
              !catalog.GetIndex("idx_empty").GetTree().GetValue(0),
              "Empty index failed or failed build consumed index ID");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Check(catalog.GetIndex(integer_index_id).GetMetadata().GetHeaderPageId() == integer_header &&
              catalog.GetIndex(bigint_index_id).GetMetadata().GetIndexName() == "idx_b" &&
              catalog.GetIndex(large_index_id).GetMetadata().GetHeaderPageId() == large_header,
              "Index metadata did not persist");
        Check(IndexedTuple(catalog, "idx_i", -7).GetValue(2) == Value::Varchar("negative") &&
              catalog.GetIndex("idx_large").GetTree().GetValue(599).has_value(),
              "Reopened B+ tree lookup failed");
        catalog.GetIndex("idx_large").GetTree().Validate();
        const auto last_index_id = catalog.GetIndex("idx_empty").GetMetadata().GetIndexId();
        engine.ExecuteSQL("DROP TABLE numbers");
        Reject<std::out_of_range>([&] { catalog.GetIndex(integer_index_id); });
        Reject<std::out_of_range>([&] { catalog.GetIndex("idx_b"); });
        Check(catalog.GetIndex("idx_large").GetMetadata().GetTableId() == large_id,
              "DROP TABLE removed another table's index");
        engine.ExecuteSQL("CREATE TABLE after_drop (id INTEGER)");
        engine.ExecuteSQL("CREATE INDEX idx_after_drop ON after_drop(id)");
        Check(catalog.GetIndex("idx_after_drop").GetMetadata().GetIndexId() > last_index_id,
              "Dropped index ID was reused");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        Reject<std::out_of_range>([&] { catalog.GetIndex("idx_i"); });
        Reject<std::out_of_range>([&] { catalog.GetIndex("idx_b"); });
        Check(catalog.GetIndex("idx_large").GetMetadata().GetIndexId() == large_index_id &&
              catalog.GetIndex("idx_large").GetTree().GetValue(-600).has_value() &&
              catalog.GetIndex("idx_after_drop").GetMetadata().GetIndexName() == "idx_after_drop",
              "DROP index cleanup or surviving index persistence failed");
        database->Close();
    }
}

void TestMetadata(const std::filesystem::path& directory) {
    const auto path = directory / "corrupt.udb";
    const auto meta = directory / "corrupt.meta";
    {
        auto database = Database::Create(path, 2);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER)");
        engine.ExecuteSQL("INSERT INTO t VALUES (7)");
        engine.ExecuteSQL("CREATE INDEX idx ON t(id)");
        database->Close();
    }
    const auto original = ReadFile(meta);
    const auto data = ReadFile(path);
    Check(original.size() == 36 && static_cast<unsigned char>(original[8]) == 6,
          "Index bootstrap metadata layout changed unexpectedly");
    auto reject_meta = [&](std::string bytes) {
        WriteFile(meta, bytes);
        Reject([&] { Database::Open(path, 1); });
        Check(ReadFile(meta) == bytes && ReadFile(path) == data, "Failed metadata Open changed files");
    };
    auto bytes = original;
    Put(bytes, 12, std::filesystem::file_size(path) / PAGE_SIZE, 8); reject_meta(bytes);
    reject_meta(original.substr(0, original.size() - 1));
    WriteFile(meta, original);

    auto corrupt_data = data;
    corrupt_data[32] ^= 1;  // Corrupt serialized Table/Column/Index catalog payload.
    WriteFile(path, corrupt_data);
    Reject([&] { Database::Open(path, 1); });
    WriteFile(path, data);
    auto database = Database::Open(path, 1);
    Check(database->GetCatalog().GetIndex("idx").GetTree().GetValue(7).has_value(),
          "Valid index metadata failed after corruption cases");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-index-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestParserBinderPlanner(directory / "binding.udb");
        TestSqlBuildAndPersistence(directory / "persistent.udb");
        TestMetadata(directory);
        std::cout << "CREATE INDEX tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
