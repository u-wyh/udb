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

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

RID Expected(std::int64_t key) {
    return RID{static_cast<page_id_t>(key + 10000), static_cast<slot_id_t>(key & 0xffff)};
}

std::vector<std::int64_t> ShuffledKeys(std::int64_t count, std::uint64_t seed) {
    std::vector<std::int64_t> keys(static_cast<std::size_t>(count));
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937_64 random(seed);
    std::shuffle(keys.begin(), keys.end(), random);
    return keys;
}

void TestLeafRedistributionAndMerge(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    BPlusTree tree(pool, BPlusTreeOptions{4, 3});
    for (std::int64_t key = 0; key < 9; ++key) {
        Check(tree.Insert(key, Expected(key)), "Leaf rebalance setup failed");
    }
    const auto free_before = disk.GetFreePageIds().size();
    Check(tree.Remove(4), "Leaf redistribution delete failed");
    tree.Validate();
    Check(disk.GetFreePageIds().size() == free_before && tree.GetValue(6),
          "Leaf redistribution unexpectedly merged a page");
    Check(tree.Remove(5), "Leaf merge delete failed");
    tree.Validate();
    Check(disk.GetFreePageIds().size() > free_before && !tree.GetValue(4) && !tree.GetValue(5),
          "Leaf merge did not release its page");
}

void TestTreeDelete(const std::filesystem::path& path) {
    constexpr std::int64_t kCount = 400;
    const BPlusTreeOptions options{4, 3};
    page_id_t header_page_id = -1;
    std::vector<std::int64_t> deletion_order = ShuffledKeys(kCount, 0x55444220);
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::CreateWithHeader(pool, options);
        header_page_id = tree->GetHeaderPageId();
        for (const auto key : ShuffledKeys(kCount, 0x42505420)) {
            Check(tree->Insert(key, Expected(key)), "B+ tree setup insert failed");
        }
        Check(tree->GetHeight() >= 4, "B+ tree setup did not reach multiple internal levels");
        Check(!tree->Remove(-1), "Removing a missing key succeeded");
        tree->Validate();
        for (std::size_t i = 0; i < 175; ++i) {
            const auto key = deletion_order[i];
            Check(tree->Remove(key) && !tree->GetValue(key), "B+ tree shuffled delete failed");
            tree->Validate();
        }
        Check(!disk.GetFreePageIds().empty(), "B+ tree merges did not release pages");
        pool.FlushAllPages();
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::OpenWithHeader(pool, header_page_id);
        tree->Validate();
        for (std::size_t i = 0; i < 175; ++i) {
            Check(!tree->GetValue(deletion_order[i]), "Deleted key returned after reopen");
        }
        for (std::size_t i = 175; i < deletion_order.size(); ++i) {
            const auto key = deletion_order[i];
            Check(tree->GetValue(key) == std::optional<RID>{Expected(key)}, "Retained key missing after reopen");
            Check(tree->Remove(key), "B+ tree delete after reopen failed");
            tree->Validate();
        }
        Check(tree->GetHeight() == 1, "B+ tree root did not collapse to an empty leaf");
        Check(!tree->GetValue(0) && !tree->Remove(0), "Empty B+ tree returned a deleted key");
        Check(tree->Insert(777, Expected(777)), "Insert after deleting all keys failed");
        tree->Validate();
        Check(tree->GetHeaderPageId() == header_page_id, "B+ tree header page changed");
        pool.FlushAllPages();
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        auto tree = BPlusTree::OpenWithHeader(pool, header_page_id);
        Check(tree->GetValue(777) == std::optional<RID>{Expected(777)},
              "Post-delete insertion was not persistent");
        tree->Validate();
    }
}

std::size_t RowCount(SqlEngine& engine) {
    return engine.ExecuteSQL("SELECT * FROM t").rows.size();
}

void ValidateTableIndexes(Catalog& catalog) {
    const auto& schema = catalog.GetTable("t").GetSchema();
    const auto& heap = catalog.GetTableHeap("t");
    for (const auto index_id : catalog.GetTableIndexes(catalog.GetTable("t").GetTableId())) {
        catalog.GetIndex(index_id).GetTree().Validate();
    }
    for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
        if (heap.GetTupleMeta(*rid).is_deleted) { continue; }
        const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), schema);
        for (const auto index_id : catalog.GetTableIndexes(catalog.GetTable("t").GetTableId())) {
            const auto& index = catalog.GetIndex(index_id);
            const auto& value = tuple.GetValue(index.GetMetadata().GetColumnIndex());
            if (value.IsNull()) { continue; }
            const auto key = value.GetType() == TypeId::INTEGER
                                 ? static_cast<std::int64_t>(value.GetInteger())
                                 : value.GetBigInt();
            Check(index.GetTree().GetValue(key) == rid, "Table row and index RID disagree");
        }
    }
}

void TestDmlMaintenance(const std::filesystem::path& path) {
    table_id_t table_id = 0;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT, n INTEGER, name VARCHAR(40))");
        table_id = catalog.GetTable("t").GetTableId();
        for (int i = 0; i < 80; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                              std::to_string(3000000000LL + i) + ", " + std::to_string(i) + ", 'row')");
        }
        catalog.CreateIndex("idx_id", table_id, 0, BPlusTreeOptions{4, 3});
        catalog.CreateIndex("idx_big", table_id, 1, BPlusTreeOptions{4, 3});
        Check(catalog.GetTableIndexes(table_id).size() == 2, "Table does not have both indexes");

        const auto inserted = engine.ExecuteSQL("INSERT INTO t VALUES (80, 5000000000, 80, 'new')");
        Check(catalog.GetIndex("idx_id").GetTree().GetValue(80) == inserted.inserted_rid &&
              catalog.GetIndex("idx_big").GetTree().GetValue(5000000000LL) == inserted.inserted_rid,
              "INSERT did not maintain every index");
        engine.ExecuteSQL("INSERT INTO t VALUES (NULL, NULL, 81, 'null')");
        Check(RowCount(engine) == 82, "NULL indexed values were not inserted");

        Reject<std::invalid_argument>([&] {
            engine.ExecuteSQL("INSERT INTO t VALUES (80, 6000000000, 82, 'duplicate')");
        });
        Check(RowCount(engine) == 82 && !catalog.GetIndex("idx_big").GetTree().GetValue(6000000000LL),
              "Rejected INSERT changed table or another index");

        const auto original_rid = *catalog.GetIndex("idx_id").GetTree().GetValue(80);
        Check(engine.ExecuteSQL("UPDATE t SET id = 180, big = 5000000001 WHERE n = 80").affected_rows == 1,
              "Indexed value-to-value UPDATE failed");
        Check(!catalog.GetIndex("idx_id").GetTree().GetValue(80) &&
              catalog.GetIndex("idx_id").GetTree().GetValue(180) == original_rid &&
              !catalog.GetIndex("idx_big").GetTree().GetValue(5000000000LL) &&
              catalog.GetIndex("idx_big").GetTree().GetValue(5000000001LL) == original_rid,
              "UPDATE did not replace indexed keys while preserving RID");
        engine.ExecuteSQL("UPDATE t SET id = NULL WHERE n = 80");
        Check(!catalog.GetIndex("idx_id").GetTree().GetValue(180), "Value-to-NULL UPDATE retained key");
        engine.ExecuteSQL("UPDATE t SET id = 181 WHERE n = 80");
        Check(catalog.GetIndex("idx_id").GetTree().GetValue(181) == original_rid,
              "NULL-to-value UPDATE did not add key");
        Check(engine.ExecuteSQL("UPDATE t SET id = NULL WHERE n = 81").affected_rows == 1,
              "NULL-to-NULL UPDATE failed");

        Reject<std::invalid_argument>([&] { engine.ExecuteSQL("UPDATE t SET id = 1 WHERE n = 80"); });
        Check(catalog.GetIndex("idx_id").GetTree().GetValue(181) == original_rid &&
              engine.ExecuteSQL("SELECT id FROM t WHERE n = 80").rows.at(0).GetValue(0) == Value::Integer(181),
              "Rejected UPDATE changed data or index");
        Reject<std::invalid_argument>([&] { engine.ExecuteSQL("UPDATE t SET id = 700 WHERE n >= 78"); });
        Check(catalog.GetIndex("idx_id").GetTree().GetValue(78) &&
              catalog.GetIndex("idx_id").GetTree().GetValue(79),
              "Multi-row unique conflict partially changed indexes");

        Check(engine.ExecuteSQL("DELETE FROM t WHERE n = 80").affected_rows == 1,
              "Indexed DELETE failed");
        Check(!catalog.GetIndex("idx_id").GetTree().GetValue(181) &&
              !catalog.GetIndex("idx_big").GetTree().GetValue(5000000001LL),
              "DELETE retained an index key");
        Check(engine.ExecuteSQL("DELETE FROM t WHERE n >= 40").affected_rows == 41,
              "Bulk indexed DELETE count wrong");
        ValidateTableIndexes(catalog);
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Check(RowCount(engine) == 40 && !catalog.GetIndex("idx_id").GetTree().GetValue(79),
              "DML index state did not survive reopen");
        ValidateTableIndexes(catalog);
        Check(engine.ExecuteSQL("DELETE FROM t WHERE n < 20").affected_rows == 20,
              "DELETE after reopen failed");
        Check(engine.ExecuteSQL("UPDATE t SET id = 220 WHERE n = 20").affected_rows == 1,
              "UPDATE after reopen failed");
        const auto inserted = engine.ExecuteSQL("INSERT INTO t VALUES (500, 9000000000, 500, 'again')");
        Check(catalog.GetIndex("idx_id").GetTree().GetValue(500) == inserted.inserted_rid,
              "INSERT after reopen did not update index");
        ValidateTableIndexes(catalog);
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Check(RowCount(engine) == 21 && !catalog.GetIndex("idx_id").GetTree().GetValue(20) &&
              catalog.GetIndex("idx_id").GetTree().GetValue(220) &&
              catalog.GetIndex("idx_id").GetTree().GetValue(500) &&
              catalog.GetIndex("idx_big").GetTree().GetValue(9000000000LL),
              "Final table/index state is wrong after second reopen");
        for (std::int64_t key = 0; key < 20; ++key) {
            Check(!catalog.GetIndex("idx_id").GetTree().GetValue(key), "Deleted key returned after second reopen");
        }
        ValidateTableIndexes(catalog);
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-index-maintenance-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestLeafRedistributionAndMerge(directory / "leaf.udb");
        TestTreeDelete(directory / "tree.udb");
        TestDmlMaintenance(directory / "database.udb");
        std::cout << "Index maintenance tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
