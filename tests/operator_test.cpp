#include "udb/sql/operator.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}
template <typename Function> void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected operator failure");
}

void Run(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    TableHeap heap(pool);
    const Schema source({Column("id", TypeId::INTEGER), Column("flag", TypeId::BOOLEAN),
                         Column("text", TypeId::VARCHAR, 2000)});
    const Schema output({source.GetColumn(2), source.GetColumn(0)});
    Check(TableScanOperator(heap, source).Execute().empty(), "Empty scan returned rows");
    for (int i = 0; i < 8; ++i) {
        const Tuple tuple(source, {Value::Integer(i), i == 7 ? Value::Null(TypeId::BOOLEAN) :
            Value::Boolean(i % 2 == 0), Value::Varchar(std::string(1500, 'x') + std::string(1, '\0'))});
        const auto rid = heap.InsertRecord(tuple.Serialize(source));
        if (i == 2) { heap.DeleteRecord(rid); }
    }
    auto flag = std::make_shared<BoundExpression>(TypeId::BOOLEAN, BoundColumnExpression{1});
    auto scan = std::make_unique<TableScanOperator>(heap, source);
    auto filter = std::make_unique<FilterOperator>(std::move(scan), flag);
    ProjectionOperator project(std::move(filter), output, {2, 0}, {});
    const auto rows = project.Execute();
    Check(rows.size() == 3 && rows[0].GetValue(1) == Value::Integer(0) &&
          rows[1].GetValue(1) == Value::Integer(4) && rows[2].GetValue(1) == Value::Integer(6),
          "Pipeline changed filter, deletion, or order semantics");
    Check(rows[0].GetValue(0).GetVarchar().size() == 1501 && rows[0].GetValue(0).GetVarchar().back() == '\0',
          "Projection lost binary bytes");
    auto null = std::make_shared<BoundExpression>(TypeId::BOOLEAN,
        BoundLiteralExpression{Value::Null(TypeId::BOOLEAN)});
    Check(FilterOperator(std::make_unique<TableScanOperator>(heap, source), null).Execute().empty(),
          "NULL filter must reject rows");
    Check(FilterOperator(std::make_unique<TableScanOperator>(heap, source), nullptr).Execute().size() == 7,
          "Absent predicate changed rows");
    auto literal = std::make_shared<BoundExpression>(TypeId::INTEGER, BoundLiteralExpression{Value::Integer(42)});
    ProjectionOperator expression(std::make_unique<TableScanOperator>(heap, source),
        Schema({Column("answer", TypeId::INTEGER)}), {}, {literal});
    Check(expression.Execute()[0].GetValue(0) == Value::Integer(42), "Expression projection failed");
    Reject([&] { FilterOperator bad(nullptr, flag); });
    Reject([&] { FilterOperator bad(std::make_unique<TableScanOperator>(heap, source), literal); });
    Reject([&] {
        ProjectionOperator bad(std::make_unique<TableScanOperator>(heap, source), output, {200, 0}, {});
        bad.Execute();
    });
    // A failed downstream operator must leave the single frame available.
    Check(TableScanOperator(heap, source).Execute().size() == 7, "Failure leaked a page pin");
    pool.FlushAllPages();
}
}  // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-operator-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        Run(directory / "database.udb");
        std::cout << "Operator tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
