#pragma once

#include "udb/page.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace udb {

// Single-owner, synchronous page I/O. Not thread-safe; do not open the same
// file through multiple managers concurrently. Flush is not an fsync guarantee.
class DiskManager {
public:
    explicit DiskManager(const std::filesystem::path& path);

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reuses the smallest free ID, or extends the file when none is free.
    // Every returned page is zero-filled.
    page_id_t AllocatePage();
    // Released pages remain physically present but cannot be read or written.
    // Invalid IDs throw out_of_range; double free throws logic_error.
    void DeallocatePage(page_id_t page_id);
    // Transaction rollback only: restore a currently free page and its image.
    void RestorePage(page_id_t page_id, const Page& page);
    // Only allocated IDs are valid. Invalid IDs throw std::out_of_range;
    // malformed files and I/O failures throw std::runtime_error.
    void WritePage(page_id_t page_id, const Page& page);
    Page ReadPage(page_id_t page_id);
    // Makes all prior data-file writes durable.
    void Sync();

    page_id_t GetPageCount() const { return page_count_; }
    page_id_t GetNextPageId() const {
        return free_pages_.empty() ? page_count_ : *free_pages_.begin();
    }
    bool IsPageAllocated(page_id_t page_id) const;
    const std::set<page_id_t>& GetFreePageIds() const { return free_pages_; }
    // Used only while opening database metadata. Validation is atomic.
    void RestoreFreePageIds(const std::vector<page_id_t>& page_ids);
    // Recovery bypasses the allocation map while replaying physical WAL, then
    // applies the resulting allocation/free state over metadata's checkpoint.
    void RecoveryWritePage(page_id_t page_id, const Page& page);
    void ApplyRecoveryPageStates(const std::map<page_id_t, bool>& allocated);

private:
    std::streamoff Offset(page_id_t page_id) const;
    void WriteAt(page_id_t page_id, const Page& page);

    std::filesystem::path path_;
    std::fstream file_;
    page_id_t page_count_ = 0;
    std::set<page_id_t> free_pages_;
};

}  // namespace udb
