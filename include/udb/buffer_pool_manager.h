#pragma once

#include "udb/disk_manager.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace udb {

// Single-threaded. DiskManager must outlive the pool; access its pages only
// through this pool while cached. Page pointers remain valid while pinned.
// Changes must be reported with UnpinPage(id, true). Flush explicitly before
// destruction: the destructor does not perform fallible I/O.
class BufferPoolManager {
public:
    BufferPoolManager(DiskManager& disk, std::size_t capacity);
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Disk errors propagate. No available unpinned frame throws runtime_error.
    Page* FetchPage(page_id_t page_id);
    std::pair<page_id_t, Page*> NewPage();
    // Missing resident IDs throw out_of_range; a zero pin count throws logic_error.
    void UnpinPage(page_id_t page_id, bool is_dirty);
    // Explicit FlushPage writes even if clean (including currently pinned data).
    // Missing resident IDs throw out_of_range. FlushAllPages writes only dirty pages.
    void FlushPage(page_id_t page_id);
    void FlushAllPages();

private:
    struct Frame {
        page_id_t page_id = -1;
        Page page;
        std::size_t pin_count = 0;
        bool dirty = false;
        bool in_use = false;
    };

    std::size_t SelectFrame() const;
    void Touch(std::size_t index);
    void WriteBack(std::size_t index);
    Page* Install(std::size_t index, page_id_t page_id, const Page& page);

    DiskManager& disk_;
    std::vector<Frame> frames_;
    std::unordered_map<page_id_t, std::size_t> page_table_;
    // All frame indices, least to most recently fetched/created. Bounded O(n)
    // scans keep LRU simple; unpinning does not count as a page access.
    std::vector<std::size_t> lru_;
};

}  // namespace udb
