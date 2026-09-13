#include "udb/transaction.h"
#include "udb/buffer_pool_manager.h"
#include "udb/log_manager.h"
#include "udb/lock_manager.h"

#include <limits>
#include <stdexcept>

namespace udb {

std::atomic<transaction_id_t> TransactionManager::next_id_{0};
std::mutex TransactionManager::timestamp_mutex_;
timestamp_t TransactionManager::last_commit_ts_ = 0;
std::map<timestamp_t, std::size_t> TransactionManager::active_read_timestamps_;

void TransactionManager::RegisterReadTimestamp(timestamp_t timestamp) {
    ++active_read_timestamps_[timestamp];
}

void TransactionManager::UnregisterReadTimestamp(timestamp_t timestamp) {
    const auto found = active_read_timestamps_.find(timestamp);
    if (found == active_read_timestamps_.end()) {
        throw std::logic_error("Transaction read timestamp is not registered");
    }
    if (--found->second == 0) { active_read_timestamps_.erase(found); }
}

TransactionManager::~TransactionManager() {
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    for (const auto& [id, transaction] : transactions_) {
        static_cast<void>(id);
        if (!transaction->IsActive()) { continue; }
        const auto found = active_read_timestamps_.find(transaction->read_ts_);
        if (found != active_read_timestamps_.end() && --found->second == 0) {
            active_read_timestamps_.erase(found);
        }
    }
}

timestamp_t TransactionManager::GetLastCommitTimestamp() {
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    return last_commit_ts_;
}

timestamp_t TransactionManager::GetWatermark() {
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    return active_read_timestamps_.empty() ? last_commit_ts_
                                           : active_read_timestamps_.begin()->first;
}

Transaction& TransactionManager::Begin(IsolationLevel isolation_level) {
    auto id = next_id_.load();
    while (true) {
        if (id == std::numeric_limits<transaction_id_t>::max()) {
            throw std::overflow_error("Transaction ID limit reached");
        }
        if (next_id_.compare_exchange_weak(id, id + 1)) { break; }
    }
    timestamp_t read_timestamp;
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        read_timestamp = last_commit_ts_;
        RegisterReadTimestamp(read_timestamp);
    }
    std::unique_ptr<Transaction> transaction;
    try {
        transaction = std::unique_ptr<Transaction>(
            new Transaction(id, isolation_level, read_timestamp));
        if (log_manager_ != nullptr) {
            log_manager_->Append(LogRecord::Begin(id));
        }
        auto* result = transaction.get();
        transactions_.emplace(id, std::move(transaction));
        return *result;
    } catch (...) {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        UnregisterReadTimestamp(read_timestamp);
        throw;
    }
}

Transaction& TransactionManager::RequireManaged(Transaction& transaction) {
    const auto found = transactions_.find(transaction.GetId());
    if (found == transactions_.end() || found->second.get() != &transaction) {
        throw std::invalid_argument("Transaction is not owned by this manager");
    }
    return *found->second;
}

void TransactionManager::Commit(Transaction& transaction) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (managed.IsAbortRequested()) { throw DeadlockError(); }
    if (pool_ != nullptr) { pool_->ThrowIfWriteError(); }
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        if (last_commit_ts_ == std::numeric_limits<timestamp_t>::max()) {
            throw std::overflow_error("Commit timestamp limit reached");
        }
        if (log_manager_ != nullptr) {
            log_manager_->Append(LogRecord::Commit(managed.GetId()));
            log_manager_->Flush();
        }
        managed.commit_ts_ = ++last_commit_ts_;
        UnregisterReadTimestamp(managed.read_ts_);
    }
    managed.before_images_.clear();
    managed.allocated_pages_.clear();
    managed.freed_pages_.clear();
    managed.state_ = TransactionState::Committed;
    if (pool_ != nullptr) { pool_->ReleaseTransactionPages(managed); }
    if (lock_manager_ != nullptr) { lock_manager_->UnlockAll(managed); }
}

void TransactionManager::Abort(Transaction& transaction,
                               const std::function<void()>& before_unlock) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (pool_ != nullptr) { pool_->RollbackTransaction(managed); }
    if (log_manager_ != nullptr) {
        log_manager_->Append(LogRecord::Abort(managed.GetId()));
        log_manager_->Flush();
    }
    managed.before_images_.clear();
    managed.allocated_pages_.clear();
    managed.freed_pages_.clear();
    managed.state_ = TransactionState::Aborted;
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        UnregisterReadTimestamp(managed.read_ts_);
    }
    if (before_unlock) { before_unlock(); }
    DiscardUndoRecords(managed);
    if (pool_ != nullptr) { pool_->ReleaseTransactionPages(managed); }
    if (lock_manager_ != nullptr) { lock_manager_->UnlockAll(managed); }
}

VersionLink TransactionManager::AppendUndoRecord(Transaction& transaction, RID rid,
                                                 const Record& record, TupleMeta meta) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (rid.page_id < 0) { throw std::invalid_argument("Undo record requires a valid RID"); }
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const auto found = version_links_.find(rid);
    const std::optional<VersionLink> previous =
        found == version_links_.end() ? std::nullopt
                                      : std::optional<VersionLink>(found->second);
    managed.undo_records_.push_back(UndoRecord{rid, record, meta, previous});
    const VersionLink link{managed.GetId(), managed.undo_records_.size() - 1};
    version_links_.insert_or_assign(rid, link);
    return link;
}

std::optional<VersionLink> TransactionManager::GetVersionLink(RID rid) const {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const auto found = version_links_.find(rid);
    return found == version_links_.end() ? std::nullopt
                                         : std::optional<VersionLink>(found->second);
}

UndoRecord TransactionManager::GetUndoRecord(VersionLink link) const {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const auto transaction = transactions_.find(link.transaction_id);
    if (transaction == transactions_.end() ||
        link.undo_index >= transaction->second->undo_records_.size()) {
        throw std::out_of_range("Undo version link does not exist");
    }
    return transaction->second->undo_records_[link.undo_index];
}

std::optional<RecordVersion> TransactionManager::ReconstructVersion(
    RID rid, const Record& current, TupleMeta current_meta,
    timestamp_t read_timestamp) const {
    if (rid.page_id < 0) {
        throw std::invalid_argument("Version reconstruction requires a valid RID");
    }
    if (current_meta.timestamp <= read_timestamp) {
        if (current_meta.is_deleted) { return std::nullopt; }
        return RecordVersion{current, current_meta};
    }

    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const auto head = version_links_.find(rid);
    std::optional<VersionLink> link =
        head == version_links_.end() ? std::nullopt
                                     : std::optional<VersionLink>(head->second);
    std::set<VersionLink> visited;
    while (link) {
        if (!visited.insert(*link).second) {
            throw std::runtime_error("Undo version chain contains a cycle");
        }
        const auto owner = transactions_.find(link->transaction_id);
        if (owner == transactions_.end() ||
            link->undo_index >= owner->second->undo_records_.size()) {
            throw std::runtime_error("Undo version chain contains a dangling link");
        }
        const auto& undo = owner->second->undo_records_[link->undo_index];
        if (undo.rid != rid) {
            throw std::runtime_error("Undo version chain points to a different RID");
        }
        if (undo.meta.timestamp <= read_timestamp) {
            if (undo.meta.is_deleted) { return std::nullopt; }
            return RecordVersion{undo.record, undo.meta};
        }
        link = undo.previous;
    }
    return std::nullopt;
}

void TransactionManager::DiscardUndoRecords(Transaction& transaction) {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    for (std::size_t i = transaction.undo_records_.size(); i != 0; --i) {
        const auto& undo = transaction.undo_records_[i - 1];
        const VersionLink discarded{transaction.GetId(), i - 1};
        const auto current = version_links_.find(undo.rid);
        if (current == version_links_.end() || current->second != discarded) {
            throw std::logic_error("Undo version chain head changed before abort");
        }
        if (undo.previous) {
            current->second = *undo.previous;
        } else {
            version_links_.erase(current);
        }
    }
    transaction.undo_records_.clear();
}

Transaction& TransactionManager::GetTransaction(transaction_id_t id) { return *transactions_.at(id); }
const Transaction& TransactionManager::GetTransaction(transaction_id_t id) const { return *transactions_.at(id); }

std::size_t TransactionManager::GetActiveCount() const {
    std::size_t count = 0;
    for (const auto& [id, transaction] : transactions_) {
        static_cast<void>(id);
        if (transaction->IsActive()) { ++count; }
    }
    return count;
}

}  // namespace udb
