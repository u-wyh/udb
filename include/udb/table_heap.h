#pragma once

#include "udb/buffer_pool_manager.h"
#include "udb/slotted_page.h"

namespace udb {

// Single-threaded, non-owning BufferPoolManager reference. Caller saves the
// first page ID and explicitly flushes the pool. No destructor I/O.
class TableHeap {
public:
    explicit TableHeap(BufferPoolManager& pool);  // Create an empty table.
    TableHeap(BufferPoolManager& pool, page_id_t first_page_id);  // Open/validate chain.

    page_id_t GetFirstPageId() const { return first_page_id_; }
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

private:
    void RequireMember(page_id_t page_id) const;
    std::optional<RID> ScanFrom(page_id_t page_id) const;

    BufferPoolManager& pool_;
    page_id_t first_page_id_;
};

}  // namespace udb
