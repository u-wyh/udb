#include "udb/buffer_pool_manager.h"
#include "udb/transaction.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace udb {

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
        ThrowIfWriteError();
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
    frame = Frame{page_id, page, 1, 0, false, true, std::nullopt};
    Touch(index);
    return &frame.page;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
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
    // Check capacity before allocating: an all-pinned failure must not grow disk.
    const auto index = SelectFrame();
    WriteBack(index);
    const auto expected_page_id = disk_.GetNextPageId();
    std::optional<lsn_t> allocation_lsn;
    if (active_transaction_ != nullptr && log_manager_ != nullptr) {
        allocation_lsn = log_manager_->Append(
            LogRecord::PageAllocate(active_transaction_->GetId(), expected_page_id));
        // AllocatePage zeroes the physical page, so its log must be durable first.
        log_manager_->Flush();
    }
    const auto page_id = disk_.AllocatePage();
    if (page_id != expected_page_id) { throw std::logic_error("Disk page allocation changed unexpectedly"); }
    if (active_transaction_ != nullptr &&
        active_transaction_->freed_pages_.count(page_id) == 0) {
        try {
            active_transaction_->allocated_pages_.insert(page_id);
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
    return ReadPageGuard(*this, page_id, FetchPage(page_id));
}

WritePageGuard BufferPoolManager::WritePage(page_id_t page_id) {
    auto* page = FetchPage(page_id);
    try {
        auto& frame = frames_[page_table_.at(page_id)];
        if (frame.write_guard_count != 0) {
            throw std::logic_error("Page already has a write guard");
        }
        if (active_transaction_ != nullptr &&
            active_transaction_->allocated_pages_.count(page_id) == 0 &&
            active_transaction_->freed_pages_.count(page_id) == 0) {
            active_transaction_->before_images_.emplace(page_id, *page);
        }
        ++frame.write_guard_count;
    } catch (...) {
        UnpinPage(page_id, false);
        throw;
    }
    return WritePageGuard(*this, page_id, page);
}

WritePageGuard BufferPoolManager::NewPageGuard() {
    const auto [page_id, page] = NewPage();
    ++frames_[page_table_.at(page_id)].write_guard_count;
    return WritePageGuard(*this, page_id, page);
}

void BufferPoolManager::CompleteWrite(page_id_t page_id, const Page& before,
                                      const Page& after) noexcept {
    const auto found = page_table_.find(page_id);
    if (found == page_table_.end()) {
        write_error_ = std::make_exception_ptr(std::logic_error("Guard page is not resident"));
        return;
    }
    auto& frame = frames_[found->second];
    const bool changed = before.data != after.data;
    if (changed && active_transaction_ != nullptr && log_manager_ != nullptr) {
        try {
            frame.page_lsn = log_manager_->Append(
                LogRecord::PageWrite(active_transaction_->GetId(), page_id, before, after));
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
    --frame.pin_count;
    frame.dirty = frame.dirty || changed;
}

void BufferPoolManager::ThrowIfWriteError() {
    if (!write_error_) { return; }
    auto error = write_error_;
    write_error_ = nullptr;
    std::rethrow_exception(error);
}

void BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    auto& frame = frames_[page_table_.at(page_id)];
    if (frame.pin_count == 0) {
        throw std::logic_error("Page is already unpinned");
    }
    --frame.pin_count;
    frame.dirty = frame.dirty || is_dirty;
}

void BufferPoolManager::FlushPage(page_id_t page_id) {
    auto& frame = frames_[page_table_.at(page_id)];
    if (frame.write_guard_count != 0) {
        throw std::logic_error("Cannot flush a page held by a write guard");
    }
    ThrowIfWriteError();
    EnsureWalDurable(frame);
    disk_.WritePage(page_id, frame.page);
    frame.dirty = false;
}

void BufferPoolManager::FlushAllPages() {
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        WriteBack(i);
    }
}

bool BufferPoolManager::CanDeletePage(page_id_t page_id) const {
    if (!disk_.IsPageAllocated(page_id)) {
        throw std::out_of_range("Page ID is not allocated");
    }
    const auto found = page_table_.find(page_id);
    return found == page_table_.end() || frames_[found->second].pin_count == 0;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    ThrowIfWriteError();
    if (!CanDeletePage(page_id)) { return false; }
    const auto found = page_table_.find(page_id);
    const auto current_page = found == page_table_.end()
                                ? disk_.ReadPage(page_id)
                                : frames_[found->second].page;
    if (active_transaction_ != nullptr && log_manager_ != nullptr) {
        log_manager_->Append(LogRecord::PageFree(active_transaction_->GetId(),
                                                 page_id, current_page));
    }
    if (active_transaction_ != nullptr) {
        if (active_transaction_->allocated_pages_.erase(page_id) == 0) {
            auto before = active_transaction_->before_images_.find(page_id);
            if (before != active_transaction_->before_images_.end()) {
                active_transaction_->freed_pages_.emplace(page_id, before->second);
                active_transaction_->before_images_.erase(before);
            } else if (found != page_table_.end()) {
                active_transaction_->freed_pages_.emplace(page_id, current_page);
            } else {
                active_transaction_->freed_pages_.emplace(page_id, current_page);
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
    frames_[index] = Frame{};
    Touch(index);
    return true;
}

void BufferPoolManager::SetActiveTransaction(Transaction* transaction) {
    if (transaction != nullptr && !transaction->IsActive()) {
        throw std::logic_error("Buffer pool transaction is not active");
    }
    if (active_transaction_ != nullptr && transaction != nullptr &&
        active_transaction_ != transaction) {
        throw std::logic_error("Another transaction is already using the buffer pool");
    }
    active_transaction_ = transaction;
}

void BufferPoolManager::RollbackTransaction(Transaction& transaction) {
    if (active_transaction_ != nullptr && active_transaction_ != &transaction) {
        throw std::logic_error("Cannot roll back a different active transaction");
    }
    active_transaction_ = nullptr;
    write_error_ = nullptr;
    if (log_manager_ != nullptr) { log_manager_->Flush(); }

    for (const auto page_id : transaction.allocated_pages_) {
        if (disk_.IsPageAllocated(page_id) && !DeletePage(page_id)) {
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
