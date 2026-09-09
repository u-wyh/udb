#pragma once

#include "udb/record.h"
#include "udb/rid.h"

#include <optional>

namespace udb {

// Non-owning view. The Page must remain alive (and pinned if in a buffer pool).
// Callers mark it dirty after mutations. No automatic initialization or I/O.
class SlottedPage {
public:
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t SLOT_SIZE = 6;

    SlottedPage(Page& page, page_id_t page_id);
    void Init();
    // nullopt means insufficient space, with no page mutation. Slot IDs are
    // never reused, so a deleted RID cannot alias a later record.
    std::optional<RID> InsertRecord(const Record& record);
    Record GetRecord(RID rid) const;
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
    std::size_t FindSlot(RID rid) const;

    Page& page_;
    page_id_t page_id_;
};

}  // namespace udb
