#pragma once

#include "udb/buffer_pool_manager.h"
#include "udb/rid.h"

#include <cstdint>
#include <optional>
#include <memory>
#include <utility>
#include <vector>

namespace udb {

struct BPlusTreeOptions {
    static constexpr std::size_t HEADER_SIZE = 24;
    static constexpr std::size_t LEAF_ENTRY_SIZE = 24;
    static constexpr std::size_t INTERNAL_ENTRY_SIZE = 16;
    static constexpr std::size_t LEAF_CAPACITY =
        (PAGE_SIZE - HEADER_SIZE) / LEAF_ENTRY_SIZE;
    static constexpr std::size_t INTERNAL_CAPACITY =
        (PAGE_SIZE - HEADER_SIZE) / INTERNAL_ENTRY_SIZE;

    std::size_t leaf_max_size = LEAF_CAPACITY;
    std::size_t internal_max_size = INTERNAL_CAPACITY;
    bool unique = true;
};

// Single-threaded persistent int64_t -> RID(s) B+ tree. All pages are accessed
// through the supplied BufferPoolManager. The caller persists root_page_id.
class BPlusTree {
public:
    explicit BPlusTree(BufferPoolManager& pool, BPlusTreeOptions options = {});
    BPlusTree(BufferPoolManager& pool, page_id_t root_page_id,
              BPlusTreeOptions options = {});
    // Header-backed trees keep a stable on-disk identity while root splits.
    static std::unique_ptr<BPlusTree> CreateWithHeader(
        BufferPoolManager& pool, BPlusTreeOptions options = {});
    static std::unique_ptr<BPlusTree> OpenWithHeader(
        BufferPoolManager& pool, page_id_t header_page_id,
        BPlusTreeOptions options = {});

    page_id_t GetRootPageId() const { return root_page_id_; }
    page_id_t GetHeaderPageId() const { return header_page_id_; }
    std::optional<RID> GetValue(std::int64_t key) const;
    std::vector<RID> GetValues(std::int64_t key) const;
    bool IsUnique() const { return options_.unique; }
    // Results are ordered by key. Missing bounds denote an open end.
    std::vector<std::pair<std::int64_t, RID>> ScanRange(
        std::optional<std::int64_t> lower, bool lower_inclusive,
        std::optional<std::int64_t> upper, bool upper_inclusive) const;
    // Unique mode rejects duplicate keys; non-unique mode rejects duplicate pairs.
    bool Insert(std::int64_t key, RID rid);
    // Removes all RIDs for the key. Missing keys return false.
    bool Remove(std::int64_t key);
    // Removes just one key/RID pair (also valid for unique trees).
    bool Remove(std::int64_t key, RID rid);
    // Explicit ownership teardown for DROP INDEX. The object is unusable after success.
    void DeletePages();

    // Full structural checks used when reopening and by storage tests.
    void Validate() const;
    std::size_t GetHeight() const;

private:
    struct Node;

    std::optional<RID> FindValue(std::int64_t key) const;
    bool InsertKey(std::int64_t key, RID rid);
    bool RemoveKey(std::int64_t key);
    std::vector<std::pair<std::int64_t, RID>> ScanEntries(
        std::optional<std::int64_t> lower, bool lower_inclusive,
        std::optional<std::int64_t> upper, bool upper_inclusive) const;
    std::vector<RID> ReadPostings(page_id_t head, std::vector<page_id_t>* pages = nullptr) const;
    page_id_t WritePostings(const std::vector<RID>& values, std::vector<page_id_t> pages);

    void ValidateOptions() const;
    Node ReadNode(page_id_t page_id) const;
    void WriteNode(page_id_t page_id, const Node& node);
    page_id_t NewNode(const Node& node);
    Page EncodeNode(const Node& node) const;
    Node DecodeNode(const Page& page, page_id_t page_id) const;
    page_id_t FindLeaf(std::int64_t key, std::vector<page_id_t>* path) const;
    void SetParent(page_id_t page_id, page_id_t parent_page_id);
    void InsertIntoParent(page_id_t left_page_id, std::int64_t separator,
                          page_id_t right_page_id, std::vector<page_id_t> path);
    std::int64_t GetMinimumKey(page_id_t page_id) const;
    void RebuildInternalKeys(Node& node) const;
    void RefreshAncestors(page_id_t page_id);
    void RebalanceAfterDelete(page_id_t page_id, Node node);
    void DeleteNode(page_id_t page_id);
    std::vector<page_id_t> CollectNodePageIds() const;
    page_id_t NewHeaderPage();
    void WriteHeader();
    static page_id_t ReadHeader(BufferPoolManager& pool, page_id_t header_page_id,
                                BPlusTreeOptions* options = nullptr);

    BufferPoolManager& pool_;
    page_id_t root_page_id_ = -1;
    page_id_t header_page_id_ = -1;
    BPlusTreeOptions options_;
};

}  // namespace udb
