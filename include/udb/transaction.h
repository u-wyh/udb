#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "udb/page.h"
#include "udb/index_key.h"
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
enum class IsolationLevel { ReadCommitted, RepeatableRead, SnapshotIsolation };

class WriteConflictError : public std::runtime_error {
public:
    WriteConflictError() : std::runtime_error("Tuple was changed after the transaction snapshot") {}
};

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
    friend bool operator<(const VersionLink& left, const VersionLink& right) {
        return std::tie(left.transaction_id, left.undo_index) <
               std::tie(right.transaction_id, right.undo_index);
    }
};

struct UndoRecord {
    RID rid;
    Record record;
    TupleMeta meta;
    std::optional<VersionLink> previous;
};

struct RecordVersion {
    Record record;
    TupleMeta meta;
};

struct StaleIndexEntry {
    std::uint64_t index_id;
    IndexKey key;
    RID rid;

    friend bool operator<(const StaleIndexEntry& left, const StaleIndexEntry& right) {
        if (left.index_id != right.index_id) { return left.index_id < right.index_id; }
        if (left.key != right.key) { return left.key < right.key; }
        return left.rid < right.rid;
    }
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
    std::set<RID> write_rids_;
    std::set<StaleIndexEntry> stale_index_entries_;
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
    static timestamp_t EncodeTransactionTimestamp(transaction_id_t transaction_id);
    static bool IsTransactionTimestamp(timestamp_t timestamp);
    static transaction_id_t DecodeTransactionTimestamp(timestamp_t timestamp);
    VersionLink AppendUndoRecord(Transaction& transaction, RID rid,
                                 const Record& record, TupleMeta meta);
    void RegisterWrite(Transaction& transaction, RID rid);
    void CheckWriteConflict(Transaction& transaction, TupleMeta current_meta);
    void RegisterStaleIndexEntry(Transaction& transaction, std::uint64_t index_id,
                                 const IndexKey& key, RID rid);
    std::vector<std::pair<IndexKey, RID>> GetStaleIndexEntries(
        std::uint64_t index_id) const;
    bool VacuumVersion(RID rid, TupleMeta current_meta, timestamp_t watermark);
    std::optional<VersionLink> GetVersionLink(RID rid) const;
    UndoRecord GetUndoRecord(VersionLink link) const;
    std::optional<RecordVersion> ReconstructVersion(RID rid, const Record& current,
                                                    TupleMeta current_meta,
                                                    timestamp_t read_timestamp,
                                                    std::optional<transaction_id_t> reader = std::nullopt) const;

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
    mutable std::mutex transactions_mutex_;
    mutable std::mutex undo_mutex_;
    std::map<RID, VersionLink> version_links_;
    std::set<StaleIndexEntry> stale_index_entries_;
    BufferPoolManager* pool_;
    LogManager* log_manager_;
    LockManager* lock_manager_;
};

}  // namespace udb
