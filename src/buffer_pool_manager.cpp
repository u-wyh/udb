#include "udb/buffer_pool_manager.h"
#include "udb/transaction.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace udb {

thread_local std::unordered_map<const BufferPoolManager*, Transaction*>
    BufferPoolManager::active_transactions_;

Transaction* BufferPoolManager::GetActiveTransaction() const {
    const auto found = active_transactions_.find(this);
    return found == active_transactions_.end() ? nullptr : found->second;
}

BufferPoolManager::BufferPoolManager(DiskManager& disk, std::size_t capacity,
                                     LogManager* log_manager)
    : disk_(disk), log_manager_(log_manager), frames_(capacity), lru_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("Buffer pool capacity must be positive");
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        lru_[i] = i;
    }
}

std::size_t BufferPoolManager::SelectFrame() const {
    for (const auto index : lru_) {
        if (!frames_[index].in_use) {
            return index;
        }
    }
    for (const auto index : lru_) {
        if (frames_[index].pin_count == 0) {
            return index;
        }
    }
    throw std::runtime_error("All buffer pool frames are pinned");
}

void BufferPoolManager::Touch(std::size_t index) {
    const auto position = std::find(lru_.begin(), lru_.end(), index);
    std::rotate(position, position + 1, lru_.end());
}

void BufferPoolManager::WriteBack(std::size_t index) {
    auto& frame = frames_[index];
    if (frame.in_use && frame.dirty) {
        ThrowIfWriteErrorLocked();
        EnsureWalDurable(frame);
        disk_.WritePage(frame.page_id, frame.page);
        frame.dirty = false;
    }
}

void BufferPoolManager::EnsureWalDurable(const Frame& frame) {
    if (!frame.page_lsn) { return; }
    if (log_manager_ == nullptr) { throw std::logic_error("Page has an LSN without a log manager"); }
    const auto persistent = log_manager_->GetPersistentLsn();
    if (!persistent || *persistent < *frame.page_lsn) { log_manager_->Flush(); }
}

Page* BufferPoolManager::Install(std::size_t index, page_id_t page_id, const Page& page) {
    auto& frame = frames_[index];
    // Allocate the new map entry first so allocation failure preserves the old
    // frame and mapping. All subsequent changes are non-throwing.
    page_table_.emplace(page_id, index);
    if (frame.in_use) {
        page_table_.erase(frame.page_id);
    }
    frame.page_id = page_id;
    frame.page = page;
    frame.pin_count = 1;
    frame.write_guard_count = 0;
    frame.dirty = false;
    frame.in_use = true;
    frame.page_lsn.reset();
    Touch(index);
    return &frame.page;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return FetchPageLocked(page_id);
}

Page* BufferPoolManager::FetchPageLocked(page_id_t page_id) {
    const auto found = page_table_.find(page_id);
    if (found != page_table_.end()) {
        auto& frame = frames_[found->second];
        if (frame.pin_count == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("Page pin count overflow");
        }
        ++frame.pin_count;
        Touch(found->second);
        return &frame.page;
    }
    const auto index = SelectFrame();
    // Validate/read before changing the victim so failed reads preserve the pool.
    const auto page = disk_.ReadPage(page_id);
    WriteBack(index);
    return Install(index, page_id, page);
}

std::pair<page_id_t, Page*> BufferPoolManager::NewPage() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return NewPageLocked();
}

std::pair<page_id_t, Page*> BufferPoolManager::NewPageLocked() {
    // Check capacity before allocating: an all-pinned failure must not grow disk.
    const auto index = SelectFrame();
    WriteBack(index);
    const auto expected_page_id = disk_.GetNextPageId();
    std::optional<lsn_t> allocation_lsn;
    auto* active_transaction = GetActiveTransaction();
    if (active_transaction != nullptr && log_manager_ != nullptr) {
        allocation_lsn = log_manager_->Append(
            LogRecord::PageAllocate(active_transaction->GetId(), expected_page_id));
        // AllocatePage zeroes the physical page, so its log must be durable first.
        log_manager_->Flush();
    }
    const auto page_id = disk_.AllocatePage();
    if (page_id != expected_page_id) { throw std::logic_error("Disk page allocation changed unexpectedly"); }
    if (active_transaction != nullptr &&
        active_transaction->freed_pages_.count(page_id) == 0) {
        try {
            active_transaction->allocated_pages_.insert(page_id);
        } catch (...) {
            disk_.DeallocatePage(page_id);
            throw;
        }
    }
    auto* page = Install(index, page_id, Page{});
    frames_[index].page_lsn = allocation_lsn;
    return {page_id, page};
}

ReadPageGuard BufferPoolManager::ReadPage(page_id_t page_id) {
    Page* page = nullptr;
    std::shared_mutex* latch = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        page = FetchPageLocked(page_id);
        latch = &frames_[page_table_.at(page_id)].latch;
    }
    try {
        std::shared_lock<std::shared_mutex> page_lock(*latch);
        return ReadPageGuard(*this, page_id, page, std::move(page_lock));
    } catch (...) {
        UnpinPage(page_id, false);
        throw;
    }
}

WritePageGuard BufferPoolManager::WritePage(page_id_t page_id) {
    Page* page = nullptr;
    std::shared_mutex* latch = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        page = FetchPageLocked(page_id);
        auto& frame = frames_[page_table_.at(page_id)];
        ++frame.write_guard_count;
        latch = &frame.latch;
    }
    try {
        std::unique_lock<std::shared_mutex> page_lock(*latch);
        const std::lock_guard<std::mutex> lock(mutex_);
        auto* active_transaction = GetActiveTransaction();
        if (active_transaction != nullptr &&
            active_transaction->allocated_pages_.count(page_id) == 0 &&
            active_transaction->freed_pages_.count(page_id) == 0) {
            active_transaction->before_images_.emplace(page_id, *page);
        }
        return WritePageGuard(*this, page_id, page, std::move(page_lock));
    } catch (...) {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto& frame = frames_[page_table_.at(page_id)];
        --frame.write_guard_count;
        UnpinPageLocked(page_id, false);
        throw;
    }
}

WritePageGuard BufferPoolManager::NewPageGuard() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto [page_id, page] = NewPageLocked();
    auto& frame = frames_[page_table_.at(page_id)];
    ++frame.write_guard_count;
    std::unique_lock<std::shared_mutex> page_lock(frame.latch);
    return WritePageGuard(*this, page_id, page, std::move(page_lock));
}

void BufferPoolManager::CompleteWrite(page_id_t page_id, const Page& before,
                                      const Page& after) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = page_table_.find(page_id);
    if (found == page_table_.end()) {
        write_error_ = std::make_exception_ptr(std::logic_error("Guard page is not resident"));
        return;
    }
    auto& frame = frames_[found->second];
    const bool changed = before.data != after.data;
    auto* active_transaction = GetActiveTransaction();
    if (changed && active_transaction != nullptr && log_manager_ != nullptr) {
        try {
            frame.page_lsn = log_manager_->Append(
                LogRecord::PageWrite(active_transaction->GetId(), page_id, before, after));
        } catch (...) {
            if (!write_error_) { write_error_ = std::current_exception(); }
        }
    }
    if (frame.pin_count == 0) {
        if (!write_error_) {
            write_error_ = std::make_exception_ptr(std::logic_error("Guard page is already unpinned"));
        }
        return;
    }
    if (frame.write_guard_count == 0) {
        if (!write_error_) {
            write_error_ = std::make_exception_ptr(std::logic_error("Page has no write guard"));
        }
    } else {
        --frame.write_guard_count;
    }
    frame.dirty = frame.dirty || changed;
}

void BufferPoolManager::ThrowIfWriteError() {
    const std::lock_guard<std::mutex> lock(mutex_);
    ThrowIfWriteErrorLocked();
}

void BufferPoolManager::ThrowIfWriteErrorLocked() {
    if (!write_error_) { return; }
    auto error = write_error_;
    write_error_ = nullptr;
    std::rethrow_exception(error);
}

void BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    const std::lock_guard<std::mutex> lock(mutex_);
    UnpinPageLocked(page_id, is_dirty);
}

void BufferPoolManager::UnpinPageLocked(page_id_t page_id, bool is_dirty) {
    auto& frame = frames_[page_table_.at(page_id)];
    if (frame.pin_count == 0) {
        throw std::logic_error("Page is already unpinned");
    }
    --frame.pin_count;
    frame.dirty = frame.dirty || is_dirty;
}

void BufferPoolManager::FlushPage(page_id_t page_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& frame = frames_[page_table_.at(page_id)];
    if (frame.write_guard_count != 0) {
        throw std::logic_error("Cannot flush a page held by a write guard");
    }
    ThrowIfWriteErrorLocked();
    EnsureWalDurable(frame);
    disk_.WritePage(page_id, frame.page);
    frame.dirty = false;
}

void BufferPoolManager::FlushAllPages() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].write_guard_count != 0) { continue; }
        WriteBack(i);
    }
}

bool BufferPoolManager::CanDeletePage(page_id_t page_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return CanDeletePageLocked(page_id);
}

bool BufferPoolManager::CanDeletePageLocked(page_id_t page_id) const {
    if (!disk_.IsPageAllocated(page_id)) {
        throw std::out_of_range("Page ID is not allocated");
    }
    const auto found = page_table_.find(page_id);
    return found == page_table_.end() || frames_[found->second].pin_count == 0;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return DeletePageLocked(page_id);
}

bool BufferPoolManager::DeletePageLocked(page_id_t page_id) {
    ThrowIfWriteErrorLocked();
    if (!CanDeletePageLocked(page_id)) { return false; }
    const auto found = page_table_.find(page_id);
    const auto current_page = found == page_table_.end()
                                ? disk_.ReadPage(page_id)
                                : frames_[found->second].page;
    auto* active_transaction = GetActiveTransaction();
    if (active_transaction != nullptr && log_manager_ != nullptr) {
        log_manager_->Append(LogRecord::PageFree(active_transaction->GetId(),
                                                 page_id, current_page));
    }
    if (active_transaction != nullptr) {
        if (active_transaction->allocated_pages_.erase(page_id) == 0) {
            auto before = active_transaction->before_images_.find(page_id);
            if (before != active_transaction->before_images_.end()) {
                active_transaction->freed_pages_.emplace(page_id, before->second);
                active_transaction->before_images_.erase(before);
            } else if (found != page_table_.end()) {
                active_transaction->freed_pages_.emplace(page_id, current_page);
            } else {
                active_transaction->freed_pages_.emplace(page_id, current_page);
            }
        }
    }
    if (found == page_table_.end()) {
        disk_.DeallocatePage(page_id);
        return true;
    }
    const auto index = found->second;
    disk_.DeallocatePage(page_id);
    page_table_.erase(found);
    auto& frame = frames_[index];
    frame.page_id = -1;
    frame.page = Page{};
    frame.pin_count = 0;
    frame.write_guard_count = 0;
    frame.dirty = false;
    frame.in_use = false;
    frame.page_lsn.reset();
    Touch(index);
    return true;
}

void BufferPoolManager::SetActiveTransaction(Transaction* transaction) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (transaction != nullptr && !transaction->IsActive()) {
        throw std::logic_error("Buffer pool transaction is not active");
    }
    auto* active_transaction = GetActiveTransaction();
    if (active_transaction != nullptr && transaction != nullptr &&
        active_transaction != transaction) {
        throw std::logic_error("Another transaction is already using the buffer pool");
    }
    if (transaction == nullptr) { active_transactions_.erase(this); }
    else { active_transactions_[this] = transaction; }
}

void BufferPoolManager::RollbackTransaction(Transaction& transaction) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto* active_transaction = GetActiveTransaction();
    if (active_transaction != nullptr && active_transaction != &transaction) {
        throw std::logic_error("Cannot roll back a different active transaction");
    }
    active_transactions_.erase(this);
    write_error_ = nullptr;
    if (log_manager_ != nullptr) { log_manager_->Flush(); }

    for (const auto page_id : transaction.allocated_pages_) {
        if (disk_.IsPageAllocated(page_id) && !DeletePageLocked(page_id)) {
            throw std::logic_error("Allocated transaction page is still pinned");
        }
    }
    for (const auto& [page_id, page] : transaction.freed_pages_) {
        if (disk_.IsPageAllocated(page_id)) {
            disk_.WritePage(page_id, page);
        } else {
            disk_.RestorePage(page_id, page);
        }
        const auto found = page_table_.find(page_id);
        if (found != page_table_.end()) {
            auto& frame = frames_[found->second];
            if (frame.pin_count != 0) {
                throw std::logic_error("Reallocated transaction page is still pinned");
            }
            frame.page = page;
            frame.dirty = false;
        }
    }
    for (const auto& [page_id, page] : transaction.before_images_) {
        disk_.WritePage(page_id, page);
        const auto found = page_table_.find(page_id);
        if (found != page_table_.end()) {
            auto& frame = frames_[found->second];
            if (frame.pin_count != 0) {
                throw std::logic_error("Transaction page is still pinned during rollback");
            }
            frame.page = page;
            frame.dirty = false;
        }
    }
}

}  // namespace udb
