#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

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
    throw std::runtime_error("Expected rejection");
}

void Verify(Database& database) {
    SqlEngine engine(database.GetCatalog());
    const std::string query = "SELECT a.id, b.name FROM a INNER JOIN b ON a.id = b.id";
    auto plan = Planner::Plan(Binder(database.GetCatalog()).Bind(Parser::Parse(query)),
                              database.GetCatalog());
    Check(plan->GetType() == PlanType::NestedLoopJoin, "Expected nested loop plan");
    const auto result = engine.ExecuteSQL(query);
    Check(result.rows.size() == 4, "Duplicate matches or NULL semantics are wrong");
    Check(result.rows[0].GetValue(1) == Value::Varchar("x") &&
          result.rows[1].GetValue(1) == Value::Varchar("y") &&
          result.rows[2].GetValue(1) == Value::Varchar("x"), "Join order is wrong");
    const auto reverse = engine.ExecuteSQL("SELECT a.id, b.name FROM a JOIN b ON b.id = a.id");
    Check(reverse.rows.size() == result.rows.size(), "Reversed ON is wrong");
    const auto cross = engine.ExecuteSQL("SELECT a.id, b.name FROM a CROSS JOIN b WHERE a.id = b.id");
    for (std::size_t i = 0; i < result.rows.size(); ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            Check(result.rows[i].GetValue(j) == cross.rows[i].GetValue(j) &&
                  result.rows[i].GetValue(j) == reverse.rows[i].GetValue(j), "Join differs from reference");
        }
    }
    const auto filtered = engine.ExecuteSQL(
        "SELECT b.name, a.id + 1 AS n FROM a JOIN b ON a.id = b.id "
        "WHERE a.flag = TRUE ORDER BY b.name DESC LIMIT 1 OFFSET 1");
    Check(filtered.rows.size() == 1 && filtered.rows[0].GetValue(0) == Value::Varchar("x") &&
          filtered.rows[0].GetValue(1) == Value::Integer(2), "Join projection/filter/pagination is wrong");
    Check(engine.ExecuteSQL("SELECT * FROM a JOIN empty ON a.id = empty.id").rows.empty(),
          "Empty right join is wrong");
    Check(engine.ExecuteSQL("SELECT * FROM empty JOIN a ON empty.id = a.id").rows.empty(),
          "Empty left join is wrong");
    Check(engine.ExecuteSQL("SELECT * FROM a JOIN b ON a.id = b.id WHERE a.id = 9").rows.empty(),
          "Unmatched key returned rows");
    const auto strings = engine.ExecuteSQL("SELECT a.id FROM a JOIN b ON a.name = b.name");
    Check(strings.rows.size() == 2, "VARCHAR join is wrong");
    const auto booleans = engine.ExecuteSQL("SELECT a.id FROM a JOIN b ON a.flag = b.flag");
    Check(booleans.rows.size() == 3, "BOOLEAN join is wrong");
    for (const auto* sql : {
        "SELECT id FROM a JOIN b ON a.id = b.id",
        "SELECT * FROM a JOIN missing ON a.id = missing.id",
        "SELECT * FROM a JOIN b ON a.id = b.name",
        "SELECT * FROM a JOIN b ON a.id = a.id",
        "SELECT * FROM a JOIN b ON a.id = 1",
        "SELECT * FROM a JOIN b ON a.id > b.id",
        "SELECT * FROM a JOIN b ON a.id = b.id OR a.flag = b.flag"}) {
        Reject<BindError>([&] { engine.ExecuteSQL(sql); });
    }
    Reject<SqlError>([&] { engine.ExecuteSQL("SELECT * FROM a JOIN b"); });
}

void Run(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());
        engine.ExecuteSQL("CREATE TABLE a (id INTEGER, name VARCHAR(3000), flag BOOLEAN)");
        engine.ExecuteSQL("CREATE TABLE b (id INTEGER, name VARCHAR(3000), flag BOOLEAN)");
        engine.ExecuteSQL("CREATE TABLE empty (id INTEGER)");
        engine.ExecuteSQL("INSERT INTO a VALUES (1, 'x', TRUE)");
        engine.ExecuteSQL("INSERT INTO a VALUES (1, 'x', FALSE)");
        engine.ExecuteSQL("INSERT INTO a VALUES (9, 'unmatched', NULL)");
        engine.ExecuteSQL("INSERT INTO a VALUES (NULL, NULL, NULL)");
        engine.ExecuteSQL("INSERT INTO b VALUES (1, 'x', TRUE)");
        engine.ExecuteSQL("INSERT INTO b VALUES (1, 'y', TRUE)");
        engine.ExecuteSQL("INSERT INTO b VALUES (2, 'z', FALSE)");
        engine.ExecuteSQL("INSERT INTO b VALUES (NULL, NULL, NULL)");
        // Force multiple pages without adding any matching join key.
        for (int i = 0; i < 4; ++i) {
            engine.ExecuteSQL("INSERT INTO b VALUES (20, '" + std::string(2500, 'q') + "', NULL)");
        }
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
            ("udb-inner-join-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        Run(directory / "database.udb");
        std::cout << "INNER JOIN tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
