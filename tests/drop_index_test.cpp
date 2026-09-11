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
    throw std::runtime_error("Expected error");
}

RID Expected(std::int64_t key) {
    return RID{key + 1, static_cast<slot_id_t>(key)};
}

void TestTreePageDeletion(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 2);
    auto tree = BPlusTree::CreateWithHeader(pool, BPlusTreeOptions{3, 3});
    for (std::int64_t key = 0; key < 100; ++key) {
        Check(tree->Insert(key, Expected(key)), "B+ tree deletion fixture insert failed");
    }
    const auto root_page_id = tree->GetRootPageId();
    const auto header_page_id = tree->GetHeaderPageId();
    const auto page_count = disk.GetPageCount();
    pool.FetchPage(root_page_id);
    Reject<std::runtime_error>([&] { tree->DeletePages(); });
    Check(disk.GetFreePageIds().empty() && tree->GetValue(50),
          "Pinned-page rejection partially deleted the B+ tree");
    pool.UnpinPage(root_page_id, false);
    tree->DeletePages();
    Check(disk.GetFreePageIds().size() == static_cast<std::size_t>(page_count) &&
          disk.GetFreePageIds().count(root_page_id) == 1 &&
          disk.GetFreePageIds().count(header_page_id) == 1,
          "B+ tree DeletePages did not reclaim every node and header page");
}

void TestCatalogDeletion(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Catalog catalog(pool);
    const Schema schema({Column("id", TypeId::INTEGER)});
    const auto table_id = catalog.CreateTable("t", schema).GetTableId();
    for (int i = 0; i < 40; ++i) {
        catalog.GetTableHeap(table_id).InsertRecord(
            Tuple(schema, {Value::Integer(i)}).Serialize(schema));
    }
    const auto& index = catalog.CreateIndex("idx", table_id, 0, BPlusTreeOptions{3, 3});
    const auto index_id = index.GetMetadata().GetIndexId();
    const auto header_page_id = index.GetMetadata().GetHeaderPageId();
    const auto root_page_id = index.GetTree().GetRootPageId();
    catalog.DropIndex(index_id);
    Check(catalog.ListIndexes().empty() && disk.GetFreePageIds().count(header_page_id) == 1 &&
          disk.GetFreePageIds().count(root_page_id) == 1,
          "Catalog DropIndex did not remove metadata and pages");
    Check(catalog.GetTableHeap(table_id).GetFirstRID().has_value(), "DropIndex damaged table data");
    Reject<std::out_of_range>([&] { catalog.GetIndex(index_id); });
    Reject<std::out_of_range>([&] { catalog.DropIndex("missing"); });

    const auto& replacement = catalog.CreateIndex("idx", table_id, 0, BPlusTreeOptions{3, 3});
    Check(replacement.GetMetadata().GetIndexId() > index_id,
          "Dropped index ID was reused");
    const auto replacement_header = replacement.GetMetadata().GetHeaderPageId();
    const auto replacement_root = replacement.GetTree().GetRootPageId();
    catalog.DropTable(table_id);
    Check(catalog.ListIndexes().empty() && catalog.ListTables().empty() &&
          disk.GetFreePageIds().count(replacement_header) == 1 &&
          disk.GetFreePageIds().count(replacement_root) == 1,
          "DropTable did not reclaim owned index pages");
}

void TestSqlAndPersistence(const std::filesystem::path& path) {
    const auto ast = std::get<DropIndexStatement>(Parser::Parse("DROP INDEX idx;"));
    Check(ast.index_name == "idx", "DROP INDEX AST is wrong");
    for (const auto sql : {"DROP INDEX", "DROP INDEX idx extra", "DROP thing"}) {
        Reject<SqlError>([&] { Parser::Parse(sql); });
    }

    index_id_t first_index_id;
    {
        auto database = Database::Create(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        engine.ExecuteSQL("CREATE TABLE t (id INTEGER, big BIGINT)");
        for (int i = 0; i < 20; ++i) {
            engine.ExecuteSQL("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                              std::to_string(3000000000LL + i) + ")");
        }
        engine.ExecuteSQL("CREATE INDEX idx ON t(id)");
        first_index_id = catalog.GetIndex("idx").GetMetadata().GetIndexId();
        const auto bound = std::get<BoundDropIndexStatement>(
            Binder(catalog).Bind(Parser::Parse("DROP INDEX idx")));
        Check(bound.index_id == first_index_id && bound.index_name == "idx",
              "DROP INDEX binding is wrong");
        const auto plan = Planner::Plan(BoundStatement{bound}, catalog);
        Check(plan->GetType() == PlanType::DropIndex &&
              dynamic_cast<const DropIndexPlan&>(*plan).GetIndexId() == first_index_id &&
              catalog.ListIndexes().size() == 1,
              "DROP INDEX planning changed Catalog or lost the index ID");
        Reject<BindError>([&] { Binder(catalog).Bind(Parser::Parse("DROP INDEX missing")); });
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id = 5").type == PlanType::IndexScan,
              "Persistent index was not restored before DROP");
        const auto result = engine.ExecuteSQL("DROP INDEX idx");
        Check(result.type == PlanType::DropIndex && catalog.ListIndexes().empty() &&
              engine.ExecuteSQL("SELECT * FROM t WHERE id = 5").type == PlanType::SeqScan,
              "SQL DROP INDEX did not remove the planner access path");
        engine.ExecuteSQL("INSERT INTO t VALUES (5, 4000000000)");
        Check(engine.ExecuteSQL("SELECT * FROM t WHERE id = 5").rows.size() == 2,
              "DML still enforced a dropped unique index");
        engine.ExecuteSQL("CREATE INDEX idx2 ON t(big)");
        Check(catalog.GetIndex("idx2").GetMetadata().GetIndexId() > first_index_id,
              "Index IDs stopped being monotonic after DROP");
        database->Close();
    }
    {
        auto database = Database::Open(path, 1);
        auto& catalog = database->GetCatalog();
        SqlEngine engine(catalog);
        Reject<std::out_of_range>([&] { catalog.GetIndex("idx"); });
        Check(catalog.ListIndexes().size() == 1 &&
              engine.ExecuteSQL("SELECT id FROM t WHERE big >= 4000000000").type ==
                  PlanType::IndexRangeScan,
              "DROP INDEX metadata or replacement index persistence is wrong");
        database->Close();
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-drop-index-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestTreePageDeletion(directory / "tree.udb");
        TestCatalogDeletion(directory / "catalog.udb");
        TestSqlAndPersistence(directory / "database.udb");
        std::cout << "DROP INDEX tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
