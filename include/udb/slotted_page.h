#pragma once

#include "udb/record.h"
#include "udb/rid.h"
#include "udb/tuple_meta.h"

#include <optional>

namespace udb {

// Non-owning view. The Page must remain alive (and pinned if in a buffer pool).
// A const Page creates a read-only view; mutators reject it. Dirty tracking and
// pin lifetime belong to the surrounding Page Guard. No automatic I/O.
class SlottedPage {
public:
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t SLOT_SIZE = 16;

    SlottedPage(Page& page, page_id_t page_id);
    SlottedPage(const Page& page, page_id_t page_id);
    void Init();
    // nullopt means insufficient space, with no page mutation. Slot IDs are
    // never reused, so a deleted RID cannot alias a later record.
    std::optional<RID> InsertRecord(const Record& record, TupleMeta meta = {});
    Record GetRecord(RID rid) const;
    TupleMeta GetTupleMeta(RID rid) const;
    void SetTupleMeta(RID rid, TupleMeta meta);
    // Returns false only when the replacement cannot fit. The page remains
    // byte-for-byte unchanged on failure; successful updates preserve all RIDs.
    bool UpdateRecord(RID rid, const Record& record);
    void DeleteRecord(RID rid);
    std::optional<RID> GetFirstRID() const;
    std::optional<RID> GetNextRID(RID current) const;  // Current must be live.
    // Maximum payload of the next insertion, excluding its new slot overhead.
    // Zero can mean no room even for a slot; InsertRecord remains authoritative.
    std::size_t GetFreeSpace() const;
    page_id_t GetNextPageId() const;
    void SetNextPageId(page_id_t page_id);  // -1 means no next page.

private:
    // Every operation validates serialized bounds/layout before accessing records.
    // Invalid/deleted RIDs throw out_of_range; corrupt layouts throw runtime_error.
    void Validate() const;
    std::size_t SerializedSlotSize() const;
    bool UpgradeLegacyFormat();
    std::size_t FindSlot(RID rid) const;
    Page& MutablePage();

    const Page& page_;
    Page* writable_;
    page_id_t page_id_;
};

}  // namespace udb
