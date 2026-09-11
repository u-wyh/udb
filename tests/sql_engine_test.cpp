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

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected SQL error");
}

void TestUnifiedExecution(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine engine(database->GetCatalog());

        const auto created = engine.ExecuteSQL(
            "CREATE TABLE users (id INTEGER, big BIGINT, active BOOLEAN, name VARCHAR(2000))");
        Check(created.type == PlanType::CreateTable && created.affected_rows == 0 && created.rows.empty(),
              "CREATE result wrong");
        engine.ExecuteSQL("CREATE TABLE empty (id INTEGER)");
        Check(engine.ExecuteSQL("SELECT * FROM empty").rows.empty(), "Empty SELECT not empty");

        Check(engine.ExecuteSQL("INSERT INTO users VALUES (NULL, NULL, NULL, NULL)").affected_rows == 1,
              "NULL INSERT failed");
        Check(engine.ExecuteSQL(u8"INSERT INTO users VALUES (-1, -2147483649, FALSE, '你好')").affected_rows == 1,
              "Typed INSERT failed");
        for (int i = 0; i < 12; ++i) {
            const auto sql = "INSERT INTO users VALUES (" + std::to_string(i) +
                ", 2147483648, TRUE, '" + std::string(1400, static_cast<char>('a' + i)) + "')";
            Check(engine.ExecuteSQL(sql).affected_rows == 1, "Multi-page INSERT failed");
        }

        const auto all = engine.ExecuteSQL("SELECT * FROM users");
        Check(all.type == PlanType::SeqScan && all.rows.size() == 14 && all.output_schema.GetColumnCount() == 4,
              "SELECT star failed");
        for (std::size_t i = 0; i < 4; ++i) {
            Check(all.rows[0].GetValue(i).IsNull(), "NULL lost");
        }
        Check(all.rows[1].GetValue(3).GetVarchar() == u8"你好", "VARCHAR lost");

        const auto projected = engine.ExecuteSQL("SELECT name, id FROM users");
        Check(projected.rows.size() == all.rows.size() && projected.output_schema.GetColumn(0).GetName() == "name" &&
              projected.output_schema.GetColumn(1).GetName() == "id", "Projection schema wrong");
        for (std::size_t i = 0; i < all.rows.size(); ++i) {
            Check(projected.rows[i].GetValue(0) == all.rows[i].GetValue(3) &&
                  projected.rows[i].GetValue(1) == all.rows[i].GetValue(0), "Projection order wrong");
        }

        Reject([&] { engine.ExecuteSQL("SELECT FROM users"); });
        Reject([&] { engine.ExecuteSQL("INSERT INTO users VALUES (1, 1, TRUE, 'unterminated)"); });
        Reject([&] { engine.ExecuteSQL("SELECT @ FROM users"); });
        Reject([&] { engine.ExecuteSQL("SELECT * FROM missing"); });
        Reject([&] { engine.ExecuteSQL("SELECT missing FROM users"); });
        Reject([&] { engine.ExecuteSQL("INSERT INTO users VALUES (TRUE, 1, TRUE, 'x')"); });
        Reject([&] { engine.ExecuteSQL("CREATE TABLE users (id INTEGER)"); });

        database->Flush();
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        SqlEngine engine(database->GetCatalog());
        const auto restored = engine.ExecuteSQL("SELECT name, id FROM users");
        Check(restored.rows.size() == 14 && restored.rows[0].GetValue(0).IsNull() &&
              restored.rows[1].GetValue(0).GetVarchar() == u8"你好", "Reopen SELECT failed");
        for (int i = 0; i < 12; ++i) {
            Check(restored.rows[i + 2].GetValue(0).GetVarchar() ==
                      std::string(1400, static_cast<char>('a' + i)) &&
                  restored.rows[i + 2].GetValue(1) == Value::Integer(i), "Reopen multi-page data wrong");
        }
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-sql-engine-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestUnifiedExecution(directory / "engine.udb");
        std::cout << "SQL engine tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
