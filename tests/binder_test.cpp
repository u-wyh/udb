#include "udb/sql/binder.h"
#include "udb/sql/parser.h"
#include "udb/tuple.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const BindError& error) {
        Check(std::string(error.what()).size() > 0, "Empty binding error");
        return;
    }
    throw std::runtime_error("Expected BindError");
}

void TestBinding(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Binder binder(catalog);
    auto bind = [&](const std::string& sql) { return binder.Bind(Parser::Parse(sql)); };
    const auto create = std::get<BoundCreateTableStatement>(bind(
        "CREATE TABLE users (id INTEGER, score BIGINT, active BOOLEAN, name VARCHAR(8))"));
    Check(create.table_name == "users" && create.schema.GetColumnCount() == 4 &&
          create.schema.GetColumn(3).GetMaxLength() == 8, "CREATE binding failed");
    Check(catalog.ListTables().empty() && std::filesystem::file_size(path) == 0, "CREATE binding performed I/O");
    Reject([&] { bind("CREATE TABLE t (id INTEGER, id BIGINT)"); });
    Reject([&] { binder.Bind(CreateTableStatement{"t", {}}); });
    Reject([&] { binder.Bind(CreateTableStatement{"", {{"id", TypeId::INTEGER, 0}}}); });
    Reject([&] { binder.Bind(CreateTableStatement{"t", {{"v", TypeId::VARCHAR, 0}}}); });
    Reject([&] { binder.Bind(CreateTableStatement{"t", {{"v", static_cast<TypeId>(255), 0}}}); });
    Reject([&] { binder.Bind(CreateTableStatement{"t", {{"v", TypeId::INTEGER, 2}}}); });
    const auto& metadata = catalog.CreateTable(create.table_name, create.schema);
    const auto before_size = std::filesystem::file_size(path);
    const auto before_tables = catalog.ListTables();
    // Keep the only frame pinned and temporarily invalidate its header. Binding
    // must still succeed because it needs metadata, not any table page access.
    auto* page = pool.FetchPage(metadata.GetFirstPageId());
    const auto saved = *page;
    page->data[0] = 0;
    Reject([&] { bind("CREATE TABLE users (x INTEGER)"); });
    const auto inserted = std::get<BoundInsertStatement>(bind("INSERT INTO users VALUES (1, 2147483648, TRUE, 'Alice')"));
    Check(inserted.table_id == metadata.GetTableId() && inserted.table_name == "users" && inserted.values.size() == 4,
          "INSERT identity/count wrong");
    Check(inserted.values[0] == Value::Integer(1) && inserted.values[1] == Value::BigInt(2147483648LL) &&
          inserted.values[2] == Value::Boolean(true) && inserted.values[3] == Value::Varchar("Alice"), "INSERT typing wrong");
    const Tuple tuple(inserted.schema, inserted.values);
    const auto decoded = Tuple::Deserialize(tuple.Serialize(inserted.schema), inserted.schema);
    Check(decoded.GetValue(1) == inserted.values[1], "Bound values incompatible with Tuple");
    const auto nulls = std::get<BoundInsertStatement>(bind("INSERT INTO users VALUES (NULL, NULL, NULL, NULL)"));
    for (std::size_t i = 0; i < 4; ++i) {
        Check(nulls.values[i].IsNull() && nulls.values[i].GetType() == create.schema.GetColumn(i).GetType(), "NULL target type wrong");
    }
    const auto limits = std::get<BoundInsertStatement>(bind("INSERT INTO users VALUES (-2147483648, -9223372036854775808, FALSE, '')"));
    Check(limits.values[0].GetInteger() == INT32_MIN && limits.values[1].GetBigInt() == INT64_MIN &&
          !limits.values[2].GetBoolean() && limits.values[3].GetVarchar().empty(), "Boundary typing wrong");
    bind("INSERT INTO users VALUES (2147483647, 9223372036854775807, FALSE, '12345678')");
    const auto utf8 = std::get<BoundInsertStatement>(bind(u8"INSERT INTO users VALUES (0, NULL, TRUE, '你好')"));
    Check(utf8.values[3].GetVarchar() == u8"你好", "UTF8 binding changed bytes");
    const auto binary = std::get<BoundInsertStatement>(binder.Bind(InsertStatement{"users",
        {std::int64_t{0}, std::monostate{}, true, std::string("a\0b", 3)}}));
    Check(binary.values[3].GetVarchar().size() == 3, "Binary literal lost bytes");
    for (const auto sql : {"INSERT INTO missing VALUES (1)", "INSERT INTO users VALUES (1)",
         "INSERT INTO users VALUES (1, NULL, TRUE, 'x', NULL)", "INSERT INTO users VALUES (2147483648, NULL, TRUE, 'x')",
         "INSERT INTO users VALUES (1, 2, TRUE, 'x')", "INSERT INTO users VALUES ('1', NULL, TRUE, 'x')",
         "INSERT INTO users VALUES (TRUE, NULL, TRUE, 'x')", "INSERT INTO users VALUES (1, NULL, 1, 'x')",
         "INSERT INTO users VALUES (1, NULL, TRUE, 1)", "INSERT INTO users VALUES (1, NULL, TRUE, '123456789')"}) {
        Reject([&] { bind(sql); });
    }
    const auto all = std::get<BoundSelectStatement>(bind("SELECT * FROM users"));
    Check(all.table_id == metadata.GetTableId() && all.table_name == "users" &&
          all.column_indexes == std::vector<std::size_t>({0, 1, 2, 3}), "Star expansion wrong");
    Check(all.output_schema.GetColumnCount() == 4 && all.output_schema.GetColumn(1).GetType() == TypeId::BIGINT,
          "Star output schema wrong");
    const auto single = std::get<BoundSelectStatement>(bind("SELECT name FROM users"));
    Check(single.column_indexes == std::vector<std::size_t>{3} && single.output_schema.GetColumn(0).GetMaxLength() == 8,
          "Single column binding wrong");
    const auto reordered = std::get<BoundSelectStatement>(bind("SELECT name, id FROM users"));
    Check(reordered.column_indexes == std::vector<std::size_t>({3, 0}) &&
          reordered.output_schema.GetColumn(0).GetName() == "name" && reordered.output_schema.GetColumn(1).GetName() == "id" &&
          reordered.output_schema.GetColumn(1).GetType() == TypeId::INTEGER, "Projection order/schema wrong");
    for (const auto sql : {"SELECT * FROM missing", "SELECT missing FROM users", "SELECT ID FROM users",
                           "SELECT * FROM Users", "SELECT id, id FROM users"}) {
        Reject([&] { bind(sql); });
    }
    Reject([&] { binder.Bind(SelectStatement{"users", false, {}, nullptr, {}, std::nullopt, 0, {}, std::nullopt, nullptr, {}}); });
    Reject([&] { binder.Bind(SelectStatement{"users", true, {"id"}, nullptr, {}, std::nullopt, 0, {}, std::nullopt, nullptr, {}}); });
    Check(catalog.ListTables() == before_tables && std::filesystem::file_size(path) == before_size && page->data[0] == 0,
          "Binder modified catalog or table pages");
    *page = saved;
    pool.UnpinPage(metadata.GetFirstPageId(), false);
    Check(!catalog.GetTableHeap("users").GetFirstRID(), "Binder executed INSERT");
    const auto& next = catalog.CreateTable("next", Schema({}));
    Check(next.GetTableId() == metadata.GetTableId() + 1, "Binder consumed table IDs");
    Check(std::get<BoundSelectStatement>(bind("SELECT * FROM next")).column_indexes.empty(), "Empty schema star failed");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-binder-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBinding(directory / "binder.udb");
        std::cout << "Binder tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
