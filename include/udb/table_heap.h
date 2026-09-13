#pragma once

#include "udb/buffer_pool_manager.h"
#include "udb/slotted_page.h"

#include <mutex>
#include <shared_mutex>
#include <vector>

namespace udb {

// Thread-safe, non-owning BufferPoolManager reference. Caller saves the first
// page ID and explicitly flushes the pool. No destructor I/O.
class TableHeap {
public:
    explicit TableHeap(BufferPoolManager& pool);  // Create an empty table.
    TableHeap(BufferPoolManager& pool, page_id_t first_page_id);  // Open/validate chain.

    page_id_t GetFirstPageId() const;
    // Oversized records throw length_error before allocating/modifying pages.
    // Buffer/I/O errors propagate. Failed append can leave an unlinked allocated
    // page; no page reclamation or transactional rollback is provided.
    RID InsertRecord(const Record& record);
    Record GetRecord(RID rid) const;
    // No cross-page relocation: false means the replacement cannot fit in the
    // RID's current page, with the original record unchanged.
    bool UpdateRecord(RID rid, const Record& record);
    void DeleteRecord(RID rid);
    std::optional<RID> GetFirstRID() const;
    // Current must be a live RID in this table; nullopt denotes end of scan.
    std::optional<RID> GetNextRID(RID current) const;
    // Explicit DROP path. Validates and unpins the full chain, verifies that no
    // page is pinned, then releases every page. Destruction alone never does this.
    void DeletePages();

private:
    std::vector<page_id_t> CollectPageIds() const;
    void RequireMember(page_id_t page_id) const;

    BufferPoolManager& pool_;
    page_id_t first_page_id_;
    mutable std::shared_mutex mutex_;
};

}  // namespace udb
