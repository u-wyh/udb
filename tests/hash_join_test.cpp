#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
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

void Compare(Catalog& catalog, const std::string& sql, std::optional<std::size_t> count = std::nullopt) {
    const auto bound = Binder(catalog).Bind(Parser::Parse(sql));
    const auto planned = Planner::Plan(bound);
    const auto& original = dynamic_cast<const JoinPlan&>(*planned);
    JoinPlan hash(original.GetLeftTableId(), original.GetRightTableId(), original.GetColumnIndexes(),
        original.GetOutputSchema(), original.GetPredicate(), original.GetOrderBy(), original.GetLimit(),
        original.GetOffset(), original.GetProjections(), original.GetJoinCondition(), JoinAlgorithm::Hash);
    Check(hash.GetType() == PlanType::HashJoin, "Wrong hash plan type");
    Executor executor(catalog);
    const auto expected = executor.Execute(*planned);
    const auto actual = executor.Execute(hash);
    Check(actual.rows.size() == expected.rows.size(), "Hash join cardinality differs");
    if (count) { Check(actual.rows.size() == *count, "Unexpected join cardinality"); }
    Check(actual.output_schema.GetColumnCount() == expected.output_schema.GetColumnCount(), "Wrong schema");
    for (std::size_t c = 0; c < actual.output_schema.GetColumnCount(); ++c) {
        Check(actual.output_schema.GetColumn(c).GetName() == expected.output_schema.GetColumn(c).GetName(),
              "Output names differ");
        for (std::size_t r = 0; r < actual.rows.size(); ++r) {
            Check(actual.rows[r].GetValue(c) == expected.rows[r].GetValue(c), "Hash join value/order differs");
        }
    }
}

void Verify(Database& database) {
    auto& catalog = database.GetCatalog();
    Compare(catalog, "SELECT l.k, r.text FROM l JOIN r ON l.k = r.k", 160);
    Compare(catalog, "SELECT r.text, l.k FROM l JOIN r ON r.k = l.k "
                     "WHERE l.flag = TRUE ORDER BY l.k DESC, r.text LIMIT 17 OFFSET 9", 17);
    Compare(catalog, "SELECT l.k FROM l JOIN r ON l.text = r.text", 80);
    Compare(catalog, "SELECT l.k FROM l JOIN r ON l.flag = r.flag");
    Compare(catalog, "SELECT l.k FROM l JOIN empty ON l.k = empty.k", 0);
    Compare(catalog, "SELECT l.k FROM empty JOIN l ON empty.k = l.k", 0);
    Compare(catalog, "SELECT l.k FROM l JOIN r ON l.k = r.k WHERE l.k = 999", 0);
    Compare(catalog, "SELECT a.k FROM a JOIN b ON a.k = b.k", 2);
    Compare(catalog, "SELECT l.k + 1 AS n FROM l JOIN r ON l.k = r.k ORDER BY l.k DESC LIMIT 2", 2);
}

void Run(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE l (k INTEGER, text VARCHAR(1000), flag BOOLEAN)");
        sql.ExecuteSQL("CREATE TABLE r (k INTEGER, text VARCHAR(1000), flag BOOLEAN)");
        sql.ExecuteSQL("CREATE TABLE empty (k INTEGER)");
        sql.ExecuteSQL("CREATE TABLE a (k BIGINT)");
        sql.ExecuteSQL("CREATE TABLE b (k BIGINT)");
        sql.ExecuteSQL("INSERT INTO a VALUES (9223372036854775807)");
        sql.ExecuteSQL("INSERT INTO a VALUES (-9223372036854775808)");
        sql.ExecuteSQL("INSERT INTO b VALUES (9223372036854775807)");
        sql.ExecuteSQL("INSERT INTO b VALUES (-9223372036854775808)");
        for (int i = 0; i < 80; ++i) {
            const auto key = std::to_string(i % 40);
            const auto payload = std::to_string(i) + std::string(600, 'x') + std::string(1, '\0') + "tail";
            const auto flag = i % 2 ? "TRUE" : "FALSE";
            for (const auto* table : {"l", "r"}) {
                sql.ExecuteSQL(std::string("INSERT INTO ") + table + " VALUES (" + key + ", '" + payload + "', " + flag + ")");
            }
        }
        sql.ExecuteSQL("INSERT INTO l VALUES (NULL, NULL, NULL)");
        sql.ExecuteSQL("INSERT INTO r VALUES (NULL, NULL, NULL)");
        Verify(*database);
        database->Close();
    }
    auto database = Database::Open(path, 1);
    Verify(*database);
    database->Close();
}
}  // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-hash-join-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        Run(directory / "database.udb");
        std::cout << "Hash join tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
