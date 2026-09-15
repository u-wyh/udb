#pragma once

#include "udb/disk_manager.h"
#include "udb/log_manager.h"
#include "udb/page_guard.h"

#include <exception>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace udb {

class Transaction;

// DiskManager must outlive the pool; access cached page contents through guards.
// Page pointers from the compatibility API remain valid while pinned but require
// caller synchronization when their contents are accessed concurrently.
// Storage code uses ReadPage/WritePage/NewPageGuard so pin ownership and dirty
// reporting are scoped. Raw methods remain for low-level tests and compatibility.
// Flush explicitly before destruction: the destructor does not perform fallible I/O.
class BufferPoolManager {
public:
    BufferPoolManager(DiskManager& disk, std::size_t capacity,
                      LogManager* log_manager = nullptr);
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Disk errors propagate. No available unpinned frame throws runtime_error.
    Page* FetchPage(page_id_t page_id);
    std::pair<page_id_t, Page*> NewPage();
    // Read guards expose const data. Write guards are the single mutable page
    // entry and conservatively mark their page dirty when released.
    ReadPageGuard ReadPage(page_id_t page_id);
    WritePageGuard WritePage(page_id_t page_id);
    WritePageGuard NewPageGuard();
    // Missing resident IDs throw out_of_range; a zero pin count throws logic_error.
    void UnpinPage(page_id_t page_id, bool is_dirty);
    // Explicit FlushPage writes even if clean. A live write guard is rejected.
    // Missing resident IDs throw out_of_range. FlushAllPages writes only dirty pages.
    void FlushPage(page_id_t page_id);
    void FlushAllPages();
    std::map<page_id_t, lsn_t> GetDirtyPageTable() const;
    // A pinned resident page cannot be deleted and leaves all state unchanged.
    // Invalid/free IDs throw through DiskManager. Dirty deleted pages are discarded.
    bool CanDeletePage(page_id_t page_id) const;
    bool DeletePage(page_id_t page_id);
    // Guard completion hook. WAL failures are deferred until the executor or a
    // flush boundary because guard destructors cannot propagate exceptions.
    void CompleteWrite(page_id_t page_id, const Page& before, const Page& after) noexcept;
    void ThrowIfWriteError();
    void SetActiveTransaction(Transaction* transaction);
    Transaction* GetActiveTransaction() const;
    void RollbackTransaction(Transaction& transaction);
    // A transaction retains ownership of every page it changes so a later
    // full-page rollback cannot erase another transaction's committed bytes.
    void ReleaseTransactionPages(Transaction& transaction);

private:
    struct Frame {
        page_id_t page_id = -1;
        Page page;
        std::size_t pin_count = 0;
        std::size_t write_guard_count = 0;
        bool dirty = false;
        bool in_use = false;
        std::optional<lsn_t> page_lsn;
        std::optional<lsn_t> rec_lsn;
        mutable std::shared_mutex latch;
    };

    std::size_t SelectFrame() const;
    void Touch(std::size_t index);
    void WriteBack(std::size_t index);
    void EnsureWalDurable(const Frame& frame);
    Page* Install(std::size_t index, page_id_t page_id, const Page& page);
    Page* FetchPageLocked(page_id_t page_id);
    std::pair<page_id_t, Page*> NewPageLocked();
    void UnpinPageLocked(page_id_t page_id, bool is_dirty);
    void ThrowIfWriteErrorLocked();
    bool CanDeletePageLocked(page_id_t page_id) const;
    bool DeletePageLocked(page_id_t page_id);
    void WaitForPageOwnerLocked(std::unique_lock<std::mutex>& lock,
                                page_id_t page_id, Transaction* transaction);
    void ClaimPageLocked(page_id_t page_id, Transaction* transaction);

    DiskManager& disk_;
    LogManager* log_manager_;
    static thread_local std::unordered_map<const BufferPoolManager*, Transaction*>
        active_transactions_;
    std::exception_ptr write_error_;
    std::vector<Frame> frames_;
    std::unordered_map<page_id_t, std::size_t> page_table_;
    std::map<page_id_t, Transaction*> page_write_owners_;
    std::condition_variable page_owner_condition_;
    // All frame indices, least to most recently fetched/created. Bounded O(n)
    // scans keep LRU simple; unpinning does not count as a page access.
    std::vector<std::size_t> lru_;
    mutable std::mutex mutex_;
};

}  // namespace udb
