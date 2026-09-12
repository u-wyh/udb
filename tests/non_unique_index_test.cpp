#include "udb/database.h"
#include "udb/sql/engine.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}

void TestTree(const std::filesystem::path& path) {
    page_id_t header = -1;
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::CreateWithHeader(pool, {3, 3, false});
        header = tree->GetHeaderPageId();
        for (int i = 0; i < 600; ++i) { Check(tree->Insert(7, RID{i, 0}), "Posting insert failed"); }
        Check(!tree->Insert(7, RID{2, 0}) && tree->GetValues(7).size() == 600, "Duplicate pair accepted");
        Check(!tree->Remove(7, RID{999, 0}) && !tree->Remove(999), "Missing removal succeeded");
        tree->Validate();
        for (int i = 0; i < 70; ++i) {
            Check(tree->Insert(i + 100, RID{i, 1}) && tree->Insert(i + 100, RID{i, 2}), "Key split failed");
        }
        tree->Validate();
        Check(tree->ScanRange(100, true, 102, true).size() == 6, "Range lost duplicate keys");
        pool.FlushAllPages();
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::OpenWithHeader(pool, header);
        Check(!tree->IsUnique() && tree->GetValues(7).size() == 600, "Non-unique mode did not persist");
        std::vector<int> order(600);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 random(123);
        std::shuffle(order.begin(), order.end(), random);
        for (const auto i : order) {
            Check(tree->Remove(7, RID{i, 0}), "Pair deletion failed");
            tree->Validate();
        }
        Check(tree->GetValues(7).empty(), "Empty key remained");
        for (int i = 0; i < 70; ++i) { Check(tree->Remove(i + 100), "Whole key deletion failed"); }
        tree->Validate();
        Check(tree->Insert(1, RID{1, 1}), "Empty tree cannot be reused");
        tree->DeletePages();
        Check(disk.GetFreePageIds().size() == static_cast<std::size_t>(disk.GetPageCount()),
              "Tree or posting pages leaked");
        auto unique = BPlusTree::CreateWithHeader(pool);
        Check(unique->IsUnique() && unique->Insert(1, RID{1, 0}) && !unique->Insert(1, RID{2, 0}),
              "Unique behavior changed");
        unique->DeletePages();
    }
}

void Verify(Database& database) {
    auto& catalog = database.GetCatalog();
    SqlEngine sql(catalog);
    for (const auto* name : {"group_index", "other_index"}) {
        const auto& index = catalog.GetIndex(name);
        Check(!index.GetTree().IsUnique(), "Catalog restored wrong index mode");
        index.GetTree().Validate();
        const auto column = index.GetMetadata().GetColumnIndex();
        const auto& schema = catalog.GetTable("t").GetSchema();
        const auto& heap = catalog.GetTableHeap("t");
        for (int key = 0; key <= 4; ++key) {
            std::vector<RID> expected;
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto row = Tuple::Deserialize(heap.GetRecord(*rid), schema);
                const auto& value = row.GetValue(column);
                if (!value.IsNull() && value.GetInteger() == key) { expected.push_back(*rid); }
            }
            auto actual = index.GetTree().GetValues(key);
            std::sort(expected.begin(), expected.end());
            std::sort(actual.begin(), actual.end());
            Check(actual == expected, "Index/table RID mismatch");
            const auto result = sql.ExecuteSQL("SELECT id FROM t WHERE " + schema.GetColumn(column).GetName() +
                                               " = " + std::to_string(key));
            Check(result.type == PlanType::IndexScan && result.rows.size() == expected.size(),
                  "Index scan lost duplicates");
        }
    }
}

void TestDatabase(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, g INTEGER, other INTEGER)");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 1, 2)");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 1, 2)");
        auto& catalog = database->GetCatalog();
        const auto id = catalog.GetTable("t").GetTableId();
        catalog.CreateIndex("pk", id, 0);
        catalog.CreateIndex("group_index", id, 1, {3, 3, false});
        catalog.CreateIndex("other_index", id, 2, {3, 3, false});
        sql.ExecuteSQL("INSERT INTO t VALUES (3, 1, NULL)");
        bool rejected = false;
        try { sql.ExecuteSQL("INSERT INTO t VALUES (1, 1, 2)"); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "Unique constraint disappeared");
        Verify(*database);
        sql.ExecuteSQL("UPDATE t SET g = 3, other = 3 WHERE id >= 1");
        Verify(*database);
        sql.ExecuteSQL("UPDATE t SET g = NULL WHERE id = 2");
        sql.ExecuteSQL("UPDATE t SET g = 3 WHERE id = 2");
        sql.ExecuteSQL("DELETE FROM t WHERE id = 1");
        Verify(*database);
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE g >= 2 AND g <= 4").rows.size() == 2,
              "Range scan lost duplicate rows");
        database->Close();
    }
    auto database = Database::Open(path, 1);
    Verify(*database);
    SqlEngine sql(database->GetCatalog());
    sql.ExecuteSQL("DELETE FROM t");
    Verify(*database);
    sql.ExecuteSQL("DROP TABLE t");
    database->Close();
}
}  // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-non-unique-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestTree(directory / "tree.udb");
        TestDatabase(directory / "database.udb");
        std::cout << "Non-unique index tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
