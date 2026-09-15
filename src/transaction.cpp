#include "udb/transaction.h"
#include "udb/buffer_pool_manager.h"
#include "udb/log_manager.h"
#include "udb/lock_manager.h"
#include "udb/slotted_page.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace udb {

namespace {
constexpr timestamp_t kTransactionTimestampBit = timestamp_t{1} << 63;
}

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

void TransactionManager::RestoreLastCommitTimestamp(timestamp_t timestamp) {
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    if (IsTransactionTimestamp(timestamp)) {
        throw std::invalid_argument("Recovered commit timestamp is invalid");
    }
    last_commit_ts_ = std::max(last_commit_ts_, timestamp);
}

Transaction& TransactionManager::Begin(IsolationLevel isolation_level) {
    auto id = next_id_.load();
    while (true) {
        if (id >= kTransactionTimestampBit) {
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
            transaction->last_lsn_ = log_manager_->Append(LogRecord::Begin(id));
        }
        auto* result = transaction.get();
        const std::lock_guard<std::mutex> transactions_lock(transactions_mutex_);
        transactions_.emplace(id, std::move(transaction));
        return *result;
    } catch (...) {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        UnregisterReadTimestamp(read_timestamp);
        throw;
    }
}

void TransactionManager::RefreshReadTimestamp(Transaction& transaction) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (managed.GetIsolationLevel() != IsolationLevel::ReadCommitted) { return; }
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    UnregisterReadTimestamp(managed.read_ts_);
    managed.read_ts_ = last_commit_ts_;
    RegisterReadTimestamp(managed.read_ts_);
}

timestamp_t TransactionManager::EncodeTransactionTimestamp(transaction_id_t transaction_id) {
    if (transaction_id >= kTransactionTimestampBit) {
        throw std::overflow_error("Transaction ID cannot be encoded as a tuple timestamp");
    }
    return kTransactionTimestampBit | transaction_id;
}

bool TransactionManager::IsTransactionTimestamp(timestamp_t timestamp) {
    return (timestamp & kTransactionTimestampBit) != 0;
}

transaction_id_t TransactionManager::DecodeTransactionTimestamp(timestamp_t timestamp) {
    if (!IsTransactionTimestamp(timestamp)) {
        throw std::invalid_argument("Tuple timestamp does not encode a transaction ID");
    }
    return timestamp & ~kTransactionTimestampBit;
}

Transaction& TransactionManager::RequireManaged(Transaction& transaction) {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(transaction.GetId());
    if (found == transactions_.end() || found->second.get() != &transaction) {
        throw std::invalid_argument("Transaction is not owned by this manager");
    }
    return *found->second;
}

bool TransactionManager::OwnsTransaction(const Transaction& transaction) const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(transaction.GetId());
    return found != transactions_.end() && found->second.get() == &transaction;
}

void TransactionManager::Commit(Transaction& transaction) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (managed.IsAbortRequested()) { throw DeadlockError(); }
    CheckSerializableCommit(managed);
    if (pool_ != nullptr) { pool_->ThrowIfWriteError(); }
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        if (last_commit_ts_ == kTransactionTimestampBit - 1) {
            throw std::overflow_error("Commit timestamp limit reached");
        }
        const auto commit_timestamp = last_commit_ts_ + 1;
        if (pool_ != nullptr && !managed.write_rids_.empty()) {
            pool_->SetActiveTransaction(&managed);
            struct ResetActive {
                BufferPoolManager& pool;
                ~ResetActive() { pool.SetActiveTransaction(nullptr); }
            } reset{*pool_};
            for (const auto rid : managed.write_rids_) {
                auto page = pool_->WritePage(rid.page_id);
                SlottedPage view(page.GetPage(), rid.page_id);
                const auto meta = view.GetTupleMeta(rid);
                if (!IsTransactionTimestamp(meta.timestamp) ||
                    DecodeTransactionTimestamp(meta.timestamp) != managed.GetId()) {
                    throw std::logic_error("Transaction no longer owns a written tuple version");
                }
                view.SetTupleMeta(rid, {commit_timestamp, meta.is_deleted});
            }
            pool_->ThrowIfWriteError();
        }
        if (log_manager_ != nullptr) {
            managed.last_lsn_ = log_manager_->Append(
                LogRecord::Commit(managed.GetId(), commit_timestamp));
            log_manager_->Flush();
        }
        last_commit_ts_ = commit_timestamp;
        managed.commit_ts_ = commit_timestamp;
        UnregisterReadTimestamp(managed.read_ts_);
    }
    managed.before_images_.clear();
    managed.allocated_pages_.clear();
    managed.freed_pages_.clear();
    managed.write_rids_.clear();
    managed.stale_index_entries_.clear();
    managed.abort_actions_.clear();
    managed.state_ = TransactionState::Committed;
    if (pool_ != nullptr) { pool_->ReleaseTransactionPages(managed); }
    if (lock_manager_ != nullptr) { lock_manager_->UnlockAll(managed); }
    GarbageCollectSsi();
}

void TransactionManager::Abort(Transaction& transaction,
                               const std::function<void()>& before_unlock) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (pool_ != nullptr) { pool_->RollbackTransaction(managed); }
    for (auto action = managed.abort_actions_.rbegin();
         action != managed.abort_actions_.rend(); ++action) {
        (*action)();
    }
    if (log_manager_ != nullptr) {
        managed.last_lsn_ = log_manager_->Append(LogRecord::Abort(managed.GetId()));
        log_manager_->Flush();
    }
    managed.before_images_.clear();
    managed.allocated_pages_.clear();
    managed.freed_pages_.clear();
    managed.write_rids_.clear();
    managed.abort_actions_.clear();
    managed.state_ = TransactionState::Aborted;
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        UnregisterReadTimestamp(managed.read_ts_);
    }
    if (before_unlock) { before_unlock(); }
    DiscardUndoRecords(managed);
    if (pool_ != nullptr) { pool_->ReleaseTransactionPages(managed); }
    if (lock_manager_ != nullptr) { lock_manager_->UnlockAll(managed); }
    GarbageCollectSsi();
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

void TransactionManager::RegisterWrite(Transaction& transaction, RID rid) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (rid.page_id < 0) { throw std::invalid_argument("Tuple write requires a valid RID"); }
    managed.write_rids_.insert(rid);
}

void TransactionManager::RegisterAbortAction(Transaction& transaction,
                                             std::function<void()> action) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (!action) { throw std::invalid_argument("Abort action must not be empty"); }
    managed.abort_actions_.push_back(std::move(action));
}

void TransactionManager::CheckWriteConflict(Transaction& transaction,
                                            TupleMeta current_meta) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (IsTransactionTimestamp(current_meta.timestamp)) {
        if (DecodeTransactionTimestamp(current_meta.timestamp) != managed.GetId()) {
            throw WriteConflictError();
        }
        return;
    }
    if (current_meta.timestamp > managed.GetReadTimestamp()) { throw WriteConflictError(); }
}

void TransactionManager::AddRwDependency(Transaction& reader, Transaction& writer) {
    if (&reader == &writer) { return; }
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto reader_found = transactions_.find(reader.GetId());
    const auto writer_found = transactions_.find(writer.GetId());
    if (reader_found == transactions_.end() || reader_found->second.get() != &reader ||
        writer_found == transactions_.end() || writer_found->second.get() != &writer) {
        throw std::invalid_argument("SSI dependency transaction is not owned by this manager");
    }
    if (reader.GetIsolationLevel() != IsolationLevel::Serializable ||
        writer.GetIsolationLevel() != IsolationLevel::Serializable) {
        throw std::invalid_argument("SSI dependency requires SERIALIZABLE transactions");
    }
    if (!writer.IsActive()) { throw std::logic_error("SSI writer transaction is not active"); }
    reader.outgoing_rw_dependencies_.insert(writer.GetId());
    writer.incoming_rw_dependencies_.insert(reader.GetId());
}

void TransactionManager::RegisterTupleRead(Transaction& reader, RID rid) {
    if (rid.page_id < 0) { throw std::invalid_argument("SIREAD requires a valid RID"); }
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(reader.GetId());
    if (found == transactions_.end() || found->second.get() != &reader) {
        throw std::invalid_argument("SIREAD transaction is not owned by this manager");
    }
    if (!reader.IsActive() || reader.GetIsolationLevel() != IsolationLevel::Serializable) {
        throw std::logic_error("SIREAD requires an active SERIALIZABLE transaction");
    }
    reader.tuple_sireads_.insert(rid);
    tuple_sireads_[rid].insert(reader.GetId());
}

void TransactionManager::RegisterTupleWrite(Transaction& writer, RID rid) {
    if (rid.page_id < 0) { throw std::invalid_argument("SSI write requires a valid RID"); }
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto writer_found = transactions_.find(writer.GetId());
    if (writer_found == transactions_.end() || writer_found->second.get() != &writer) {
        throw std::invalid_argument("SSI writer is not owned by this manager");
    }
    if (!writer.IsActive() || writer.GetIsolationLevel() != IsolationLevel::Serializable) { return; }
    const auto reads = tuple_sireads_.find(rid);
    if (reads == tuple_sireads_.end()) { return; }
    for (const auto reader_id : reads->second) {
        if (reader_id == writer.GetId()) { continue; }
        const auto reader_found = transactions_.find(reader_id);
        if (reader_found == transactions_.end()) { continue; }
        auto& reader = *reader_found->second;
        if (reader.GetState() == TransactionState::Aborted ||
            reader.GetIsolationLevel() != IsolationLevel::Serializable) { continue; }
        const bool concurrent = reader.IsActive() ||
            (reader.GetCommitTimestamp() &&
             *reader.GetCommitTimestamp() > writer.GetReadTimestamp());
        if (!concurrent) { continue; }
        reader.outgoing_rw_dependencies_.insert(writer.GetId());
        writer.incoming_rw_dependencies_.insert(reader.GetId());
    }
}

std::size_t TransactionManager::GetTupleSireadCount() const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    std::size_t count = 0;
    for (const auto& [rid, readers] : tuple_sireads_) {
        static_cast<void>(rid);
        count += readers.size();
    }
    return count;
}

void TransactionManager::RegisterTableRead(Transaction& reader, table_id_t table_id) {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(reader.GetId());
    if (found == transactions_.end() || found->second.get() != &reader ||
        !reader.IsActive() || reader.GetIsolationLevel() != IsolationLevel::Serializable) {
        throw std::logic_error("Table SIREAD requires an active SERIALIZABLE transaction");
    }
    PredicateSiread read{reader.GetId(), table_id, std::nullopt,
                         std::nullopt, true, std::nullopt, true};
    reader.predicate_sireads_.insert(read);
    predicate_sireads_.insert(std::move(read));
}

void TransactionManager::RegisterIndexRead(
    Transaction& reader, table_id_t table_id, std::uint64_t index_id,
    std::optional<IndexKey> lower, bool lower_inclusive,
    std::optional<IndexKey> upper, bool upper_inclusive) {
    if (lower && upper && *upper < *lower) {
        throw std::invalid_argument("SIREAD index range is reversed");
    }
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(reader.GetId());
    if (found == transactions_.end() || found->second.get() != &reader ||
        !reader.IsActive() || reader.GetIsolationLevel() != IsolationLevel::Serializable) {
        throw std::logic_error("Predicate SIREAD requires an active SERIALIZABLE transaction");
    }
    PredicateSiread read{reader.GetId(), table_id, index_id,
                         std::move(lower), lower_inclusive,
                         std::move(upper), upper_inclusive};
    reader.predicate_sireads_.insert(read);
    predicate_sireads_.insert(std::move(read));
}

namespace {
bool SsiConcurrent(const Transaction& reader, const Transaction& writer) {
    return reader.IsActive() ||
           (reader.GetCommitTimestamp() &&
            *reader.GetCommitTimestamp() > writer.GetReadTimestamp());
}

bool Contains(const PredicateSiread& read, const IndexKey& key) {
    if (read.lower && (key < *read.lower ||
                       (key == *read.lower && !read.lower_inclusive))) { return false; }
    if (read.upper && (key > *read.upper ||
                       (key == *read.upper && !read.upper_inclusive))) { return false; }
    return true;
}
}  // namespace

void TransactionManager::RegisterTableWrite(Transaction& writer, table_id_t table_id) {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto writer_found = transactions_.find(writer.GetId());
    if (writer_found == transactions_.end() || writer_found->second.get() != &writer) {
        throw std::invalid_argument("SSI writer is not owned by this manager");
    }
    if (!writer.IsActive() || writer.GetIsolationLevel() != IsolationLevel::Serializable) { return; }
    for (const auto& read : predicate_sireads_) {
        if (read.table_id != table_id || read.index_id || read.reader_id == writer.GetId()) { continue; }
        const auto found = transactions_.find(read.reader_id);
        if (found == transactions_.end() || found->second->GetState() == TransactionState::Aborted ||
            !SsiConcurrent(*found->second, writer)) { continue; }
        found->second->outgoing_rw_dependencies_.insert(writer.GetId());
        writer.incoming_rw_dependencies_.insert(read.reader_id);
    }
}

void TransactionManager::RegisterIndexWrite(Transaction& writer, table_id_t table_id,
                                             std::uint64_t index_id,
                                             const IndexKey& key) {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto writer_found = transactions_.find(writer.GetId());
    if (writer_found == transactions_.end() || writer_found->second.get() != &writer) {
        throw std::invalid_argument("SSI writer is not owned by this manager");
    }
    if (!writer.IsActive() || writer.GetIsolationLevel() != IsolationLevel::Serializable) { return; }
    for (const auto& read : predicate_sireads_) {
        if (read.table_id != table_id || read.index_id != index_id ||
            read.reader_id == writer.GetId() || !Contains(read, key)) { continue; }
        const auto found = transactions_.find(read.reader_id);
        if (found == transactions_.end() || found->second->GetState() == TransactionState::Aborted ||
            !SsiConcurrent(*found->second, writer)) { continue; }
        found->second->outgoing_rw_dependencies_.insert(writer.GetId());
        writer.incoming_rw_dependencies_.insert(read.reader_id);
    }
}

std::size_t TransactionManager::GetPredicateSireadCount() const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    return predicate_sireads_.size();
}

bool TransactionManager::HasDangerousStructure(const Transaction& transaction) const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    const auto found = transactions_.find(transaction.GetId());
    if (found == transactions_.end() || found->second.get() != &transaction) {
        throw std::invalid_argument("SSI transaction is not owned by this manager");
    }
    return transaction.GetIsolationLevel() == IsolationLevel::Serializable &&
           !transaction.incoming_rw_dependencies_.empty() &&
           !transaction.outgoing_rw_dependencies_.empty();
}

void TransactionManager::CheckSerializableCommit(const Transaction& transaction) const {
    if (HasDangerousStructure(transaction)) { throw SerializationFailure(); }
}

std::size_t TransactionManager::GetRetainedSsiTransactionCount() const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    return static_cast<std::size_t>(std::count_if(
        transactions_.begin(), transactions_.end(), [](const auto& entry) {
            return entry.second->ssi_metadata_retained_;
        }));
}

std::size_t TransactionManager::GarbageCollectSsi() {
    const auto watermark = GetWatermark();
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    std::set<transaction_id_t> expired;
    for (const auto& [id, transaction] : transactions_) {
        if (!transaction->ssi_metadata_retained_ || transaction->IsActive()) { continue; }
        if (transaction->GetState() == TransactionState::Aborted ||
            (transaction->commit_ts_ && *transaction->commit_ts_ <= watermark)) {
            expired.insert(id);
        }
    }
    if (expired.empty()) { return 0; }

    for (auto read = predicate_sireads_.begin(); read != predicate_sireads_.end();) {
        if (expired.count(read->reader_id) != 0) { read = predicate_sireads_.erase(read); }
        else { ++read; }
    }
    for (auto read = tuple_sireads_.begin(); read != tuple_sireads_.end();) {
        for (const auto id : expired) { read->second.erase(id); }
        if (read->second.empty()) { read = tuple_sireads_.erase(read); }
        else { ++read; }
    }
    for (auto& [id, transaction] : transactions_) {
        static_cast<void>(id);
        for (const auto expired_id : expired) {
            transaction->incoming_rw_dependencies_.erase(expired_id);
            transaction->outgoing_rw_dependencies_.erase(expired_id);
        }
    }
    for (const auto id : expired) {
        auto& transaction = *transactions_.at(id);
        transaction.incoming_rw_dependencies_.clear();
        transaction.outgoing_rw_dependencies_.clear();
        transaction.tuple_sireads_.clear();
        transaction.predicate_sireads_.clear();
        transaction.ssi_metadata_retained_ = false;
    }
    return expired.size();
}

void TransactionManager::RegisterStaleIndexEntry(Transaction& transaction,
                                                 std::uint64_t index_id,
                                                 const IndexKey& key, RID rid) {
    auto& managed = RequireManaged(transaction);
    if (!managed.IsActive()) { throw std::logic_error("Transaction is not active"); }
    if (rid.page_id < 0) { throw std::invalid_argument("Stale index entry requires a valid RID"); }
    const StaleIndexEntry entry{index_id, key, rid};
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    if (stale_index_entries_.insert(entry).second) {
        managed.stale_index_entries_.insert(entry);
    }
}

std::vector<std::pair<IndexKey, RID>> TransactionManager::GetStaleIndexEntries(
    std::uint64_t index_id) const {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    std::vector<std::pair<IndexKey, RID>> result;
    for (const auto& entry : stale_index_entries_) {
        if (entry.index_id < index_id) { continue; }
        if (entry.index_id > index_id) { break; }
        result.emplace_back(entry.key, entry.rid);
    }
    return result;
}

bool TransactionManager::VacuumVersion(RID rid, TupleMeta current_meta,
                                       timestamp_t watermark) {
    if (IsTransactionTimestamp(current_meta.timestamp)) { return false; }
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const std::lock_guard<std::mutex> transactions_lock(transactions_mutex_);
    const auto head = version_links_.find(rid);
    if (current_meta.timestamp <= watermark) {
        if (head != version_links_.end()) { version_links_.erase(head); }
        for (auto entry = stale_index_entries_.begin(); entry != stale_index_entries_.end();) {
            if (entry->rid == rid) { entry = stale_index_entries_.erase(entry); }
            else { ++entry; }
        }
        return current_meta.is_deleted;
    }
    if (head == version_links_.end()) { return false; }
    auto link = head->second;
    std::set<VersionLink> visited;
    while (visited.insert(link).second) {
        const auto owner = transactions_.find(link.transaction_id);
        if (owner == transactions_.end() || link.undo_index >= owner->second->undo_records_.size()) {
            throw std::runtime_error("Vacuum found a dangling undo link");
        }
        auto& undo = owner->second->undo_records_[link.undo_index];
        if (!IsTransactionTimestamp(undo.meta.timestamp) && undo.meta.timestamp <= watermark) {
            undo.previous.reset();
            break;
        }
        if (!undo.previous) { break; }
        link = *undo.previous;
    }
    return false;
}

std::optional<VersionLink> TransactionManager::GetVersionLink(RID rid) const {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const auto found = version_links_.find(rid);
    return found == version_links_.end() ? std::nullopt
                                         : std::optional<VersionLink>(found->second);
}

UndoRecord TransactionManager::GetUndoRecord(VersionLink link) const {
    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const std::lock_guard<std::mutex> transactions_lock(transactions_mutex_);
    const auto transaction = transactions_.find(link.transaction_id);
    if (transaction == transactions_.end() ||
        link.undo_index >= transaction->second->undo_records_.size()) {
        throw std::out_of_range("Undo version link does not exist");
    }
    return transaction->second->undo_records_[link.undo_index];
}

std::optional<RecordVersion> TransactionManager::ReconstructVersion(
    RID rid, const Record& current, TupleMeta current_meta,
    timestamp_t read_timestamp, std::optional<transaction_id_t> reader) const {
    if (rid.page_id < 0) {
        throw std::invalid_argument("Version reconstruction requires a valid RID");
    }
    const auto visible = [&](TupleMeta meta) {
        if (IsTransactionTimestamp(meta.timestamp)) {
            return reader && DecodeTransactionTimestamp(meta.timestamp) == *reader;
        }
        return meta.timestamp <= read_timestamp;
    };
    if (visible(current_meta)) {
        if (current_meta.is_deleted) { return std::nullopt; }
        return RecordVersion{current, current_meta};
    }

    const std::lock_guard<std::mutex> lock(undo_mutex_);
    const std::lock_guard<std::mutex> transactions_lock(transactions_mutex_);
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
        if (visible(undo.meta)) {
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
    for (const auto& entry : transaction.stale_index_entries_) {
        stale_index_entries_.erase(entry);
    }
    transaction.stale_index_entries_.clear();
    transaction.undo_records_.clear();
}

Transaction& TransactionManager::GetTransaction(transaction_id_t id) {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    return *transactions_.at(id);
}
const Transaction& TransactionManager::GetTransaction(transaction_id_t id) const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    return *transactions_.at(id);
}

std::size_t TransactionManager::GetActiveCount() const {
    const std::lock_guard<std::mutex> lock(transactions_mutex_);
    std::size_t count = 0;
    for (const auto& [id, transaction] : transactions_) {
        static_cast<void>(id);
        if (transaction->IsActive()) { ++count; }
    }
    return count;
}

}  // namespace udb
