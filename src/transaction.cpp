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
    if (pool_ != nullptr) { pool_->ReleaseTransactionPages(managed); }
    if (lock_manager_ != nullptr) { lock_manager_->UnlockAll(managed); }
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
