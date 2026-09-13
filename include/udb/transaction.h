#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "udb/page.h"
#include "udb/rid.h"
#include "udb/table_metadata.h"

namespace udb {

class BufferPoolManager;
class LogManager;
class LockManager;

using transaction_id_t = std::uint64_t;

enum class TransactionState { Active, Committed, Aborted };

struct RowLockId {
    table_id_t table_id;
    RID rid;

    friend bool operator<(const RowLockId& left, const RowLockId& right) {
        if (left.table_id != right.table_id) { return left.table_id < right.table_id; }
        return left.rid < right.rid;
    }
};

class Transaction {
public:
    transaction_id_t GetId() const { return id_; }
    TransactionState GetState() const { return state_; }
    bool IsActive() const { return state_ == TransactionState::Active; }
    bool IsAbortRequested() const { return abort_requested_.load(); }
    const std::set<table_id_t>& GetSharedTableLocks() const { return shared_table_locks_; }
    const std::set<table_id_t>& GetExclusiveTableLocks() const { return exclusive_table_locks_; }
    const std::set<RowLockId>& GetSharedRowLocks() const { return shared_row_locks_; }
    const std::set<RowLockId>& GetExclusiveRowLocks() const { return exclusive_row_locks_; }

private:
    friend class TransactionManager;
    friend class BufferPoolManager;
    friend class LockManager;
    explicit Transaction(transaction_id_t id) : id_(id) {}
    transaction_id_t id_;
    TransactionState state_ = TransactionState::Active;
    std::atomic<bool> abort_requested_{false};
    std::map<page_id_t, Page> before_images_;
    std::set<page_id_t> allocated_pages_;
    std::map<page_id_t, Page> freed_pages_;
    std::set<table_id_t> shared_table_locks_;
    std::set<table_id_t> exclusive_table_locks_;
    std::set<RowLockId> shared_row_locks_;
    std::set<RowLockId> exclusive_row_locks_;
};

class TransactionManager {
public:
    explicit TransactionManager(BufferPoolManager* pool = nullptr,
                                LogManager* log_manager = nullptr,
                                LockManager* lock_manager = nullptr)
        : pool_(pool), log_manager_(log_manager), lock_manager_(lock_manager) {}
    Transaction& Begin();
    void Commit(Transaction& transaction);
    void Abort(Transaction& transaction, const std::function<void()>& before_unlock = {});
    Transaction& GetTransaction(transaction_id_t id);
    const Transaction& GetTransaction(transaction_id_t id) const;
    std::size_t GetActiveCount() const;

private:
    Transaction& RequireManaged(Transaction& transaction);
    static std::atomic<transaction_id_t> next_id_;
    std::map<transaction_id_t, std::unique_ptr<Transaction>> transactions_;
    BufferPoolManager* pool_;
    LogManager* log_manager_;
    LockManager* lock_manager_;
};

}  // namespace udb
