#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "udb/page.h"
#include "udb/record.h"
#include "udb/rid.h"
#include "udb/table_metadata.h"
#include "udb/tuple_meta.h"

namespace udb {

class BufferPoolManager;
class LogManager;
class LockManager;

using transaction_id_t = std::uint64_t;
using timestamp_t = std::uint64_t;

enum class TransactionState { Active, Committed, Aborted };
enum class IsolationLevel { ReadCommitted, RepeatableRead };

struct VersionLink {
    transaction_id_t transaction_id;
    std::size_t undo_index;

    friend bool operator==(const VersionLink& left, const VersionLink& right) {
        return left.transaction_id == right.transaction_id &&
               left.undo_index == right.undo_index;
    }
    friend bool operator!=(const VersionLink& left, const VersionLink& right) {
        return !(left == right);
    }
};

struct UndoRecord {
    RID rid;
    Record record;
    TupleMeta meta;
    std::optional<VersionLink> previous;
};

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
    IsolationLevel GetIsolationLevel() const { return isolation_level_; }
    timestamp_t GetReadTimestamp() const { return read_ts_; }
    std::optional<timestamp_t> GetCommitTimestamp() const { return commit_ts_; }
    std::size_t GetUndoRecordCount() const { return undo_records_.size(); }
    const std::set<table_id_t>& GetSharedTableLocks() const { return shared_table_locks_; }
    const std::set<table_id_t>& GetExclusiveTableLocks() const { return exclusive_table_locks_; }
    const std::set<RowLockId>& GetSharedRowLocks() const { return shared_row_locks_; }
    const std::set<RowLockId>& GetExclusiveRowLocks() const { return exclusive_row_locks_; }

private:
    friend class TransactionManager;
    friend class BufferPoolManager;
    friend class LockManager;
    Transaction(transaction_id_t id, IsolationLevel isolation_level, timestamp_t read_ts)
        : id_(id), isolation_level_(isolation_level), read_ts_(read_ts) {}
    transaction_id_t id_;
    IsolationLevel isolation_level_;
    timestamp_t read_ts_;
    std::optional<timestamp_t> commit_ts_;
    TransactionState state_ = TransactionState::Active;
    std::atomic<bool> abort_requested_{false};
    std::map<page_id_t, Page> before_images_;
    std::set<page_id_t> allocated_pages_;
    std::map<page_id_t, Page> freed_pages_;
    std::set<table_id_t> shared_table_locks_;
    std::set<table_id_t> exclusive_table_locks_;
    std::set<RowLockId> shared_row_locks_;
    std::set<RowLockId> exclusive_row_locks_;
    std::vector<UndoRecord> undo_records_;
};

class TransactionManager {
public:
    explicit TransactionManager(BufferPoolManager* pool = nullptr,
                                LogManager* log_manager = nullptr,
                                LockManager* lock_manager = nullptr)
        : pool_(pool), log_manager_(log_manager), lock_manager_(lock_manager) {}
    ~TransactionManager();
    Transaction& Begin(IsolationLevel isolation_level = IsolationLevel::RepeatableRead);
    void Commit(Transaction& transaction);
    void Abort(Transaction& transaction, const std::function<void()>& before_unlock = {});
    Transaction& GetTransaction(transaction_id_t id);
    const Transaction& GetTransaction(transaction_id_t id) const;
    std::size_t GetActiveCount() const;
    static timestamp_t GetLastCommitTimestamp();
    static timestamp_t GetWatermark();
    VersionLink AppendUndoRecord(Transaction& transaction, RID rid,
                                 const Record& record, TupleMeta meta);
    std::optional<VersionLink> GetVersionLink(RID rid) const;
    UndoRecord GetUndoRecord(VersionLink link) const;

private:
    Transaction& RequireManaged(Transaction& transaction);
    static std::atomic<transaction_id_t> next_id_;
    static std::mutex timestamp_mutex_;
    static timestamp_t last_commit_ts_;
    static std::map<timestamp_t, std::size_t> active_read_timestamps_;
    static void RegisterReadTimestamp(timestamp_t timestamp);
    static void UnregisterReadTimestamp(timestamp_t timestamp);
    void DiscardUndoRecords(Transaction& transaction);
    std::map<transaction_id_t, std::unique_ptr<Transaction>> transactions_;
    mutable std::mutex undo_mutex_;
    std::map<RID, VersionLink> version_links_;
    BufferPoolManager* pool_;
    LogManager* log_manager_;
    LockManager* lock_manager_;
};

}  // namespace udb
