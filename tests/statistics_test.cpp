#include "udb/database.h"
#include "udb/sql/engine.h"

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

void CheckPopulated(const TableStatistics& statistics) {
    Check(statistics.row_count == 5 && statistics.columns.size() == 5,
          "Table statistics shape is wrong");
    const auto& id = statistics.columns[0];
    Check(id.null_count == 1 && id.non_null_count == 4 && id.distinct_count == 3 &&
          id.minimum == Value::Integer(-2) && id.maximum == Value::Integer(9),
          "INTEGER statistics are wrong");
    const auto& big = statistics.columns[1];
    Check(big.null_count == 1 && big.distinct_count == 3 &&
          big.minimum == Value::BigInt(-3000000000LL) &&
          big.maximum == Value::BigInt(5000000000LL), "BIGINT statistics are wrong");
    const auto& flag = statistics.columns[2];
    Check(flag.null_count == 1 && flag.distinct_count == 2 &&
          flag.minimum == Value::Boolean(false) && flag.maximum == Value::Boolean(true),
          "BOOLEAN statistics are wrong");
    const auto& name = statistics.columns[3];
    Check(name.null_count == 1 && name.distinct_count == 3 &&
          name.minimum == Value::Varchar(std::string("a\0b", 3)) &&
          name.maximum == Value::Varchar("z"), "VARCHAR statistics are wrong");
    const auto& score = statistics.columns[4];
    Check(score.null_count == 1 && score.distinct_count == 3 &&
          score.minimum == Value::Double(-1.5) && score.maximum == Value::Double(8.25),
          "DOUBLE statistics are wrong");
}

void TestStatistics(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        const Schema schema({Column("id", TypeId::INTEGER), Column("big", TypeId::BIGINT),
            Column("flag", TypeId::BOOLEAN), Column("name", TypeId::VARCHAR, 20),
            Column("score", TypeId::DOUBLE)});
        const auto first = catalog.CreateTable("first", schema).GetTableId();
        const auto empty = catalog.CreateTable("empty", schema).GetTableId();
        auto& heap = catalog.GetTableHeap(first);
        const auto insert = [&](std::vector<Value> values) {
            heap.InsertRecord(Tuple(schema, std::move(values)).Serialize(schema));
        };
        insert({Value::Integer(-2), Value::BigInt(-3000000000LL), Value::Boolean(false),
                Value::Varchar(std::string("a\0b", 3)), Value::Double(-1.5)});
        insert({Value::Integer(9), Value::BigInt(5000000000LL), Value::Boolean(true),
                Value::Varchar("z"), Value::Double(8.25)});
        insert({Value::Integer(9), Value::BigInt(7), Value::Boolean(true),
                Value::Varchar("same"), Value::Double(2.0)});
        insert({Value::Integer(1), Value::BigInt(7), Value::Boolean(false),
                Value::Varchar("same"), Value::Double(2.0)});
        insert({Value::Null(TypeId::INTEGER), Value::Null(TypeId::BIGINT),
                Value::Null(TypeId::BOOLEAN), Value::Null(TypeId::VARCHAR),
                Value::Null(TypeId::DOUBLE)});

        Check(!catalog.HasTableStatistics(first), "New table unexpectedly has statistics");
        Reject<std::logic_error>([&] { catalog.GetTableStatistics(first); });
        CheckPopulated(catalog.AnalyzeTable("first"));
        Check(&catalog.GetTableStatistics(first) == &catalog.GetTableStatistics("first"),
              "Statistics lookup does not return the catalog snapshot");
        const auto& empty_statistics = catalog.AnalyzeTable(empty);
        Check(empty_statistics.row_count == 0 && empty_statistics.columns.size() == 5,
              "Empty table statistics are wrong");
        for (const auto& column : empty_statistics.columns) {
            Check(column.null_count == 0 && column.non_null_count == 0 &&
                  column.distinct_count == 0 && !column.minimum && !column.maximum,
                  "Empty column statistics are wrong");
        }

        insert({Value::Integer(20), Value::BigInt(20), Value::Boolean(true),
                Value::Varchar("new"), Value::Double(20.0)});
        Check(catalog.GetTableStatistics(first).row_count == 5,
              "Explicit statistics snapshot changed without ANALYZE");
        Check(catalog.AnalyzeTable(first).row_count == 6 &&
              catalog.GetTableStatistics(first).columns[0].maximum == Value::Integer(20),
              "Re-analysis did not refresh statistics");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        const auto table = catalog.GetTable("first").GetTableId();
        Check(!catalog.HasTableStatistics(table), "Statistics were unexpectedly persisted");
        Check(catalog.AnalyzeTable(table).row_count == 6,
              "Statistics could not be rebuilt after reopen");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-statistics-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestStatistics(directory / "statistics.udb");
        std::cout << "Statistics tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
