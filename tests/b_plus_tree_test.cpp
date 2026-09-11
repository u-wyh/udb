#include "udb/b_plus_tree.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

RID Expected(std::int64_t key) {
    const auto shifted = static_cast<std::uint64_t>(key) ^ (std::uint64_t{1} << 63);
    return RID{static_cast<page_id_t>(shifted % 1000000), static_cast<slot_id_t>(shifted % 65536)};
}

void CheckUnpinned(BufferPoolManager& pool, page_id_t page_id) {
    pool.FetchPage(page_id);
    pool.UnpinPage(page_id, false);
    Reject<std::logic_error>([&] { pool.UnpinPage(page_id, false); });
}

void TestBasic(const std::filesystem::path& path) {
    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    Check(BPlusTreeOptions{}.leaf_max_size ==
              (PAGE_SIZE - BPlusTreeOptions::HEADER_SIZE) / BPlusTreeOptions::LEAF_ENTRY_SIZE,
          "Default leaf capacity is not derived from PAGE_SIZE");
    Reject<std::invalid_argument>([&] { BPlusTree tree(pool, BPlusTreeOptions{1, 3}); });
    Reject<std::invalid_argument>([&] { BPlusTree tree(pool, BPlusTreeOptions{3, 1}); });
    BPlusTree tree(pool, BPlusTreeOptions{3, 3});
    const auto initial_root = tree.GetRootPageId();
    Check(initial_root == 0 && tree.GetHeight() == 1 && !tree.GetValue(0), "New B+ tree is not empty");

    const std::vector<std::int64_t> keys = {
        40, -5, 90, 0, 12, 11, 10, 100, -100, 13, 14, 15, 16, 17, 18,
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    for (const auto key : keys) {
        Check(tree.Insert(key, Expected(key)), "Unique B+ tree insert failed");
    }
    const auto pages_before_duplicate = disk.GetPageCount();
    Check(!tree.Insert(12, RID{999, 1}) && disk.GetPageCount() == pages_before_duplicate,
          "Duplicate key changed the B+ tree");
    for (const auto key : keys) {
        Check(tree.GetValue(key) == std::optional<RID>{Expected(key)}, "B+ tree lookup returned wrong RID");
    }
    Check(!tree.GetValue(19) && !tree.GetValue(-99), "Missing B+ tree key was found");
    tree.Validate();
    Check(tree.GetHeight() >= 3 && tree.GetRootPageId() != initial_root,
          "Leaf/internal/root splits did not create a multi-level tree");
    CheckUnpinned(pool, tree.GetRootPageId());
    Reject<std::invalid_argument>([&] { BPlusTree reopened(pool, page_id_t{-1}); });
    Reject<std::out_of_range>([&] { BPlusTree reopened(pool, page_id_t{9999}); });
}

std::vector<std::int64_t> Keys(std::int64_t first, std::int64_t count) {
    std::vector<std::int64_t> keys(static_cast<std::size_t>(count));
    std::iota(keys.begin(), keys.end(), first);
    std::mt19937_64 random(0x554442);
    std::shuffle(keys.begin(), keys.end(), random);
    return keys;
}

void Verify(BPlusTree& tree, std::int64_t first, std::int64_t count) {
    tree.Validate();
    for (std::int64_t key = first; key < first + count; ++key) {
        Check(tree.GetValue(key) == std::optional<RID>{Expected(key)}, "Persistent B+ tree lookup failed");
    }
}

void TestLargeAndPersistent(const std::filesystem::path& path) {
    const BPlusTreeOptions options{16, 8};
    page_id_t root;
    std::uintmax_t first_size;
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        BPlusTree tree(pool, options);
        for (const auto key : Keys(-1500, 3000)) {
            Check(tree.Insert(key, Expected(key)), "Large shuffled insert failed");
        }
        Verify(tree, -1500, 3000);
        Check(tree.GetHeight() >= 3, "Large B+ tree did not split internal nodes");
        root = tree.GetRootPageId();
        CheckUnpinned(pool, root);
        pool.FlushAllPages();
        first_size = std::filesystem::file_size(path);
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        BPlusTree tree(pool, root, options);
        Verify(tree, -1500, 3000);
        for (const auto key : Keys(1500, 1000)) {
            Check(tree.Insert(key, Expected(key)), "Insert after reopen failed");
        }
        Verify(tree, -1500, 4000);
        Check(!tree.GetValue(-1501) && !tree.GetValue(2500), "Reopened tree found missing key");
        root = tree.GetRootPageId();
        pool.FlushAllPages();
        Check(std::filesystem::file_size(path) >= first_size, "Continued insert shrank B+ tree file");
    }
    {
        DiskManager disk(path);
        BufferPoolManager pool(disk, 1);
        BPlusTree tree(pool, root, options);
        Verify(tree, -1500, 4000);
        CheckUnpinned(pool, root);
    }
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("udb-bpt-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestBasic(directory / "basic.udb");
        TestLargeAndPersistent(directory / "persistent.udb");
        std::cout << "B+ tree tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
