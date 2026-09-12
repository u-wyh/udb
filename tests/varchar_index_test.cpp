#include "udb/database.h"
#include "udb/sql/engine.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}

void TestTree(const std::filesystem::path& path, bool unique) {
    std::vector<std::string> keys = {"", "a", std::string("a\0", 2), std::string("a\0b", 3),
                                   std::string(1, static_cast<char>(255))};
    for (int i = 0; i < 150; ++i) { keys.push_back("same-prefix-" + std::to_string(i)); }
    page_id_t header;
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::CreateWithHeader(pool, {3, 3, unique, 32});
        header = tree->GetHeaderPageId();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            Check(tree->Insert(IndexKey(keys[i]), RID{static_cast<page_id_t>(i), 0}), "String insert failed");
            tree->Validate();
        }
        Check(tree->Insert(IndexKey(keys[0]), RID{999, 0}) == !unique, "String uniqueness failed");
        bool rejected = false;
        try { tree->Insert(IndexKey(std::string(33, 'x')), RID{999, 0}); }
        catch (const std::length_error&) { rejected = true; }
        Check(rejected, "Oversized key accepted");
        const auto entries = tree->ScanKeys(std::nullopt, true, std::nullopt, true);
        Check(entries.size() == keys.size() + (unique ? 0U : 1U), "String range lost entries");
        for (std::size_t i = 1; i < entries.size(); ++i) {
            Check(entries[i - 1].first <= entries[i].first, "String range ordering failed");
        }
        Check(tree->ScanKeys(IndexKey("a"), true, IndexKey(std::string("a\0b", 3)), true).size() == 3,
              "Binary range bounds failed");
        pool.FlushAllPages();
    }
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    auto tree = BPlusTree::OpenWithHeader(pool, header);
    Check(tree->IsUnique() == unique && tree->GetStringMaxLength() == 32, "String format did not persist");
    for (std::size_t i = 0; i < keys.size(); ++i) {
        Check(tree->GetValue(IndexKey(keys[i])) == RID{static_cast<page_id_t>(i), 0}, "Reopen lookup failed");
    }
    std::mt19937 random(42);
    std::shuffle(keys.begin(), keys.end(), random);
    for (const auto& key : keys) { Check(tree->Remove(IndexKey(key)), "String remove failed"); tree->Validate(); }
    Check(tree->Insert(IndexKey("again"), RID{1, 0}), "String empty tree cannot be reused");
    tree->DeletePages();
    Check(disk.GetFreePageIds().size() == static_cast<std::size_t>(disk.GetPageCount()), "String pages leaked");
}

void TestSql(const std::filesystem::path& path) {
    {
        auto database = Database::Create(path, 1);
        SqlEngine sql(database->GetCatalog());
        sql.ExecuteSQL("CREATE TABLE t (id INTEGER, name VARCHAR(20), tag VARCHAR(20))");
        sql.ExecuteSQL("INSERT INTO t VALUES (1, 'alice', 'same')");
        sql.ExecuteSQL("INSERT INTO t VALUES (2, 'bob', 'same')");
        sql.ExecuteSQL("CREATE INDEX names ON t(name)");
        auto& catalog = database->GetCatalog();
        catalog.CreateIndex("tags", catalog.GetTable("t").GetTableId(), 2, {3, 3, false});
        auto rows = sql.ExecuteSQL("SELECT id FROM t WHERE name = 'alice'");
        Check(rows.type == PlanType::IndexScan && rows.rows.size() == 1, "VARCHAR equality did not use index");
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE 'bob' = name").rows[0].GetValue(0) == Value::Integer(2),
              "Reversed VARCHAR equality failed");
        rows = sql.ExecuteSQL("SELECT name FROM t WHERE name >= 'a' AND name < 'c' ORDER BY name DESC");
        Check(rows.type == PlanType::IndexRangeScan && rows.rows.size() == 2 &&
              rows.rows[0].GetValue(0) == Value::Varchar("bob"), "VARCHAR range failed");
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE tag = 'same'").rows.size() == 2, "Non-unique VARCHAR failed");
        bool rejected = false;
        try { sql.ExecuteSQL("INSERT INTO t VALUES (3, 'alice', 'same')"); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "String unique constraint failed");
        sql.ExecuteSQL("UPDATE t SET name = 'carol' WHERE id = 1");
        sql.ExecuteSQL("UPDATE t SET tag = NULL WHERE id = 2");
        sql.ExecuteSQL("INSERT INTO t VALUES (3, NULL, 'same')");
        const auto long_query = sql.ExecuteSQL("SELECT id FROM t WHERE name = '" + std::string(30, 'z') + "'");
        Check(long_query.type == PlanType::SeqScan && long_query.rows.empty(), "Oversized literal changed semantics");
        const auto binary = std::string("x\0y", 3);
        sql.ExecuteSQL("INSERT INTO t VALUES (4, '" + binary + "', 'binary')");
        Check(sql.ExecuteSQL("SELECT id FROM t WHERE name = '" + binary + "'").rows[0].GetValue(0) == Value::Integer(4),
              "Binary indexed lookup failed");
        sql.ExecuteSQL("CREATE TABLE wide (s VARCHAR(1025))");
        rejected = false;
        try { sql.ExecuteSQL("CREATE INDEX too_wide ON wide(s)"); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected, "Oversized indexed column accepted");
        catalog.GetIndex("names").GetTree().Validate();
        catalog.GetIndex("tags").GetTree().Validate();
        database->Close();
    }
    auto database = Database::Open(path, 1);
    SqlEngine sql(database->GetCatalog());
    Check(sql.ExecuteSQL("SELECT id FROM t WHERE name = 'carol'").rows.size() == 1, "Reopen SQL string lookup failed");
    Check(sql.ExecuteSQL("SELECT id FROM t WHERE tag = 'same'").rows.size() == 2, "Reopen postings failed");
    sql.ExecuteSQL("DELETE FROM t WHERE name = 'carol'");
    Check(sql.ExecuteSQL("SELECT id FROM t WHERE name = 'carol'").rows.empty(), "String delete did not update index");
    sql.ExecuteSQL("DROP TABLE t");
    database->Close();
}

void TestMaximumWidth(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    BPlusTreeOptions options;
    options.string_max_length = 1024;
    auto tree = BPlusTree::CreateWithHeader(pool, options);
    for (int i = 0; i < 30; ++i) {
        auto key = std::string(1024, 'x');
        key[0] = static_cast<char>(i);
        Check(tree->Insert(IndexKey(key), RID{i, 0}), "Maximum-width insert failed");
        tree->Validate();
    }
    Check(tree->GetHeight() >= 3, "Dynamic key capacity did not split nodes");
    tree->DeletePages();
}
}  // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("udb-varchar-index-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestTree(directory / "unique.udb", true);
        TestTree(directory / "multi.udb", false);
        TestSql(directory / "database.udb");
        TestMaximumWidth(directory / "wide.udb");
        std::cout << "VARCHAR index tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
