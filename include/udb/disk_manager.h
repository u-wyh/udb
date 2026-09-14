#pragma once

#include "udb/page.h"
#include "udb/lsn.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace udb {

// Synchronous, mutex-protected page I/O. Do not open the same file through
// multiple managers concurrently.
class DiskManager {
public:
    explicit DiskManager(const std::filesystem::path& path);
    static std::filesystem::path GetPageLsnPath(const std::filesystem::path& data_path);

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
    void WritePage(page_id_t page_id, const Page& page,
                   std::optional<lsn_t> page_lsn = std::nullopt);
    Page ReadPage(page_id_t page_id);
    std::optional<lsn_t> GetPageLsn(page_id_t page_id) const;
    void SetPageLsn(page_id_t page_id, lsn_t page_lsn);
    // Makes all prior data-file writes durable.
    void Sync();

    page_id_t GetPageCount() const;
    page_id_t GetNextPageId() const;
    bool IsPageAllocated(page_id_t page_id) const;
    std::set<page_id_t> GetFreePageIds() const;
    // Used only while opening database metadata. Validation is atomic.
    void RestoreFreePageIds(const std::vector<page_id_t>& page_ids);
    // Recovery bypasses the allocation map while replaying physical WAL, then
    // applies the resulting allocation/free state over metadata's checkpoint.
    void RecoveryWritePage(page_id_t page_id, const Page& page,
                           std::optional<lsn_t> page_lsn = std::nullopt);
    void ApplyRecoveryPageStates(const std::map<page_id_t, bool>& allocated);

private:
    std::streamoff Offset(page_id_t page_id) const;
    bool IsPageAllocatedUnlocked(page_id_t page_id) const;
    void WriteAt(page_id_t page_id, const Page& page);
    void OpenPageLsnStore();
    void WritePageLsnUnlocked(page_id_t page_id, std::optional<lsn_t> page_lsn);
    void SyncDataUnlocked();
    void SyncPageLsnUnlocked();

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::fstream file_;
    std::filesystem::path page_lsn_path_;
    std::fstream page_lsn_file_;
    std::vector<std::optional<lsn_t>> page_lsns_;
    page_id_t page_count_ = 0;
    std::set<page_id_t> free_pages_;
};

}  // namespace udb
