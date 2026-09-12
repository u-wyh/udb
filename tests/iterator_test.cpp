#include "udb/database.h"
#include "udb/sql/engine.h"
#include "udb/sql/operator.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}

class CountingSource final : public ExecutionOperator {
public:
    explicit CountingSource(std::size_t& calls) : calls_(calls) {}
    void Init() override { calls_ = 0; position_ = 0; initialized_ = true; }
    std::optional<Tuple> Next() override {
        RequireInitialized();
        ++calls_;
        if (position_ == 1000000) { return std::nullopt; }
        return Tuple(Schema({Column("n", TypeId::INTEGER)}), {Value::Integer(position_++)});
    }
private:
    std::size_t& calls_;
    std::int32_t position_ = 0;
};

void TestLazyPipeline() {
    std::size_t calls = 0;
    auto input = std::make_unique<CountingSource>(calls);
    auto column = std::make_shared<BoundExpression>(TypeId::INTEGER, BoundColumnExpression{0});
    auto two = std::make_shared<BoundExpression>(TypeId::INTEGER, BoundLiteralExpression{Value::Integer(2)});
    auto predicate = std::make_shared<BoundExpression>(TypeId::BOOLEAN,
        BoundComparisonExpression{ComparisonOperator::GreaterEqual, column, two});
    auto filter = std::make_unique<FilterOperator>(std::move(input), predicate);
    auto limit = std::make_unique<LimitOperator>(std::move(filter), 2, 1);
    ProjectionOperator pipeline(std::move(limit), Schema({Column("n", TypeId::INTEGER)}), {0}, {});
    bool rejected = false;
    try { pipeline.Next(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Next before Init must fail");
    pipeline.Init();
    Check(calls == 0, "Init eagerly consumed input");
    Check(pipeline.Next()->GetValue(0) == Value::Integer(3) && calls == 4, "First row is not lazy");
    Check(pipeline.Next()->GetValue(0) == Value::Integer(4) && calls == 5, "Second row is wrong");
    Check(!pipeline.Next() && !pipeline.Next() && calls == 5, "Limit consumed excess input");
    pipeline.Init();
    Check(pipeline.Next()->GetValue(0) == Value::Integer(3), "Init did not reset pipeline");
    const auto rows = pipeline.Execute();
    Check(rows.size() == 2 && calls == 5, "Compatibility adapter is wrong");
    LimitOperator zero(std::make_unique<CountingSource>(calls), 0, 100);
    Check(zero.Execute().empty() && calls == 0, "LIMIT 0 consumed input");
}

void TestHeap(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("CREATE TABLE t (id INTEGER, text VARCHAR(2000))");
    for (int i = 0; i < 8; ++i) {
        sql.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + std::string(1500, 'x') + "')");
    }
    auto& catalog = database->GetCatalog();
    TableScanOperator scan(catalog.GetTableHeap("t"), catalog.GetTable("t").GetSchema());
    scan.Init();
    Check(scan.Next()->GetValue(0) == Value::Integer(0), "First scan row is wrong");
    // Interleave another table operation with an unfinished scan at capacity 1.
    sql.ExecuteSQL("CREATE TABLE other (id INTEGER)");
    Check(scan.Next()->GetValue(0) == Value::Integer(1), "Interleaved scan leaked pins");
    scan.Init();
    Check(scan.Next()->GetValue(0) == Value::Integer(0), "Scan reset failed");
    auto rows = sql.ExecuteSQL("SELECT id FROM t WHERE id >= 2 LIMIT 2 OFFSET 1").rows;
    Check(rows.size() == 2 && rows[0].GetValue(0) == Value::Integer(3), "SQL iterator pagination is wrong");
    rows = sql.ExecuteSQL("SELECT id FROM t ORDER BY id DESC LIMIT 1").rows;
    Check(rows[0].GetValue(0) == Value::Integer(7), "Blocking sort behavior changed");
    database->Close();
}
}  // namespace

int main() {
    try {
        TestLazyPipeline();
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-iterator-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestHeap(directory / "database.udb");
        std::cout << "Iterator tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
