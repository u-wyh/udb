#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void CheckOne(const ExecutionResult& result, const Value& value, const char* message) {
    Check(result.rows.size() == 1 && result.rows[0].GetValue(0) == value, message);
}

void TestMvccIndexOnlyScan(Catalog& catalog) {
    SqlEngine setup(catalog);
    setup.ExecuteSQL("CREATE TABLE t (id INTEGER, group_id INTEGER, name VARCHAR(40))");
    setup.ExecuteSQL("INSERT INTO t VALUES (1, 5, 'old')");
    setup.ExecuteSQL("INSERT INTO t VALUES (2, 5, 'second')");
    setup.ExecuteSQL("CREATE INDEX id_idx ON t(id)");
    setup.ExecuteSQL("CREATE INDEX name_idx ON t(name)");
    BPlusTreeOptions non_unique;
    non_unique.unique = false;
    catalog.CreateIndex("group_idx", catalog.GetTable("t").GetTableId(), 1, non_unique);

    const auto bound = Binder(catalog).Bind(
        Parser::Parse("SELECT id FROM t WHERE id = 1"));
    const auto plan = Planner::Plan(bound, catalog);
    Check(plan->GetType() == PlanType::IndexOnlyScan,
          "Covered equality did not plan as IndexOnlyScan");

    SqlEngine old(catalog);
    old.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    CheckOne(old.ExecuteSQL("SELECT id FROM t WHERE id = 1"), Value::Integer(1),
             "Visible committed equality covering scan failed");
    const auto range = old.ExecuteSQL("SELECT id FROM t WHERE id >= 1 AND id <= 2").rows;
    Check(range.size() == 2 && range[0].GetValue(0) == Value::Integer(1) &&
              range[1].GetValue(0) == Value::Integer(2),
          "Visible committed range covering scan failed");
    const auto groups = old.ExecuteSQL("SELECT group_id FROM t WHERE group_id = 5").rows;
    Check(groups.size() == 2 && groups[0].GetValue(0) == Value::Integer(5) &&
              groups[1].GetValue(0) == Value::Integer(5),
          "Visible non-unique covering scan failed");
    CheckOne(old.ExecuteSQL("SELECT name FROM t WHERE name = 'old'"), Value::Varchar("old"),
             "Visible VARCHAR covering scan failed");

    SqlEngine writer(catalog);
    writer.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    writer.ExecuteSQL("UPDATE t SET id = 10, name = 'new' WHERE id = 1");
    CheckOne(writer.ExecuteSQL("SELECT id FROM t WHERE id = 10"), Value::Integer(10),
             "Writer cannot cover its own current version");
    CheckOne(old.ExecuteSQL("SELECT id FROM t WHERE id = 1"), Value::Integer(1),
             "Stale index entry did not safely fall back to the old tuple");
    Check(old.ExecuteSQL("SELECT id FROM t WHERE id = 10").rows.empty(),
          "Future uncommitted index entry bypassed tuple visibility");
    writer.ExecuteSQL("COMMIT");
    CheckOne(old.ExecuteSQL("SELECT name FROM t WHERE name = 'old'"), Value::Varchar("old"),
             "Committed future version bypassed old-snapshot fallback");
    Check(old.ExecuteSQL("SELECT name FROM t WHERE name = 'new'").rows.empty(),
          "Committed future covering key leaked into old snapshot");
    CheckOne(setup.ExecuteSQL("SELECT id FROM t WHERE id = 10"), Value::Integer(10),
             "Fresh snapshot cannot cover current committed version");
    old.ExecuteSQL("COMMIT");

    SqlEngine before_delete(catalog);
    before_delete.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    SqlEngine deleter(catalog);
    deleter.ExecuteSQL("BEGIN ISOLATION LEVEL SNAPSHOT");
    deleter.ExecuteSQL("DELETE FROM t WHERE id = 10");
    CheckOne(before_delete.ExecuteSQL("SELECT id FROM t WHERE id = 10"), Value::Integer(10),
             "Uncommitted tombstone hid a visible stale index version");
    deleter.ExecuteSQL("COMMIT");
    CheckOne(before_delete.ExecuteSQL("SELECT id FROM t WHERE id = 10"), Value::Integer(10),
             "Committed tombstone hid an older visible index version");
    before_delete.ExecuteSQL("COMMIT");
    Check(setup.ExecuteSQL("SELECT id FROM t WHERE id = 10").rows.empty(),
          "Fresh index-only scan returned a committed tombstone");

    for (const auto index_id : catalog.GetTableIndexes(catalog.GetTable("t").GetTableId())) {
        catalog.GetIndex(index_id).GetTree().Validate();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-mvcc-index-only-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        auto database = Database::Create(directory / "database.udb", 3);
        TestMvccIndexOnlyScan(database->GetCatalog());
        database->Close();
        std::cout << "MVCC index-only scan tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
