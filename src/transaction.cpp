#include "udb/transaction.h"
#include "udb/buffer_pool_manager.h"
#include "udb/log_manager.h"

#include <limits>
#include <stdexcept>

namespace udb {

std::atomic<transaction_id_t> TransactionManager::next_id_{0};

Transaction& TransactionManager::Begin() {
    auto id = next_id_.load();
    while (true) {
        if (id == std::numeric_limits<transaction_id_t>::max()) {
            throw std::overflow_error("Transaction ID limit reached");
        }
        if (next_id_.compare_exchange_weak(id, id + 1)) { break; }
    }
    auto transaction = std::unique_ptr<Transaction>(new Transaction(id));
    auto* result = transaction.get();
    if (log_manager_ != nullptr) {
        log_manager_->Append(LogRecord::Begin(id));
    }
    transactions_.emplace(id, std::move(transaction));
    return *result;
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
    if (pool_ != nullptr) { pool_->ThrowIfWriteError(); }
    if (log_manager_ != nullptr) {
        log_manager_->Append(LogRecord::Commit(managed.GetId()));
        log_manager_->Flush();
    }
    managed.before_images_.clear();
    managed.allocated_pages_.clear();
    managed.freed_pages_.clear();
    managed.state_ = TransactionState::Committed;
}

void TransactionManager::Abort(Transaction& transaction) {
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
