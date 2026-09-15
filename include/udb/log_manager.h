#pragma once

#include "udb/page.h"
#include "udb/lsn.h"
#include "udb/transaction.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <vector>

namespace udb {

enum class LogRecordType : std::uint8_t {
    Begin = 0,
    PageWrite = 1,
    PageAllocate = 2,
    PageFree = 3,
    Commit = 4,
    Abort = 5,
    Compensation = 6
};

enum class CompensationType : std::uint8_t { PageWrite = 0, PageAllocate = 1, PageFree = 2 };

struct LogCheckpoint {
    lsn_t checkpoint_lsn = 0;
    std::map<transaction_id_t, lsn_t> transaction_table;
    std::map<page_id_t, lsn_t> dirty_page_table;
};

class LogRecord {
public:
    static LogRecord Begin(transaction_id_t transaction_id);
    static LogRecord PageWrite(transaction_id_t transaction_id, page_id_t page_id,
                               const Page& before, const Page& after);
    static LogRecord PageAllocate(transaction_id_t transaction_id, page_id_t page_id);
    static LogRecord PageFree(transaction_id_t transaction_id, page_id_t page_id,
                              const Page& before);
    static LogRecord Commit(transaction_id_t transaction_id,
                            std::optional<timestamp_t> commit_timestamp = std::nullopt);
    static LogRecord Abort(transaction_id_t transaction_id);
    static LogRecord Compensation(transaction_id_t transaction_id, page_id_t page_id,
                                  CompensationType compensation_type, const Page& after,
                                  std::optional<lsn_t> undo_next_lsn);

    lsn_t GetLsn() const { return lsn_; }
    transaction_id_t GetTransactionId() const { return transaction_id_; }
    LogRecordType GetType() const { return type_; }
    std::optional<page_id_t> GetPageId() const { return page_id_; }
    const std::optional<Page>& GetBeforeImage() const { return before_image_; }
    const std::optional<Page>& GetAfterImage() const { return after_image_; }
    std::optional<timestamp_t> GetCommitTimestamp() const { return commit_timestamp_; }
    std::optional<lsn_t> GetPrevLsn() const { return prev_lsn_; }
    std::optional<lsn_t> GetUndoNextLsn() const { return undo_next_lsn_; }
    std::optional<CompensationType> GetCompensationType() const {
        return compensation_type_;
    }
    void Validate() const;

private:
    friend class LogManager;
    friend class LogRecordDecoder;
    LogRecord(LogRecordType type, transaction_id_t transaction_id,
              std::optional<page_id_t> page_id = std::nullopt,
              std::optional<Page> before = std::nullopt,
              std::optional<Page> after = std::nullopt,
              std::optional<timestamp_t> commit_timestamp = std::nullopt,
              std::optional<lsn_t> prev_lsn = std::nullopt,
              std::optional<lsn_t> undo_next_lsn = std::nullopt,
              std::optional<CompensationType> compensation_type = std::nullopt)
        : transaction_id_(transaction_id), type_(type), page_id_(page_id),
          before_image_(std::move(before)), after_image_(std::move(after)),
          commit_timestamp_(commit_timestamp), prev_lsn_(prev_lsn),
          undo_next_lsn_(undo_next_lsn), compensation_type_(compensation_type) {}
    lsn_t lsn_ = 0;
    transaction_id_t transaction_id_;
    LogRecordType type_;
    std::optional<page_id_t> page_id_;
    std::optional<Page> before_image_;
    std::optional<Page> after_image_;
    std::optional<timestamp_t> commit_timestamp_;
    std::optional<lsn_t> prev_lsn_;
    std::optional<lsn_t> undo_next_lsn_;
    std::optional<CompensationType> compensation_type_;
};

// Append-only, single-threaded WAL foundation. Append assigns contiguous LSNs;
// Flush makes records visible through the stream's durable boundary. Record
// framing, version, lengths, sequence and checksums are validated on open.
class LogManager {
public:
    explicit LogManager(const std::filesystem::path& path);
    static std::filesystem::path GetSequencePath(const std::filesystem::path& wal_path);
    static std::filesystem::path GetCheckpointPath(const std::filesystem::path& wal_path);
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    lsn_t Append(LogRecord record);
    void Flush();
    bool HasActiveTransactions() const;
    std::map<transaction_id_t, lsn_t> GetActiveTransactionTable() const;
    void WriteCheckpoint(const LogCheckpoint& checkpoint);
    std::optional<LogCheckpoint> ReadCheckpoint() const;
    std::size_t TruncateForCheckpoint(const LogCheckpoint& checkpoint);
    // Used after a checkpoint has safely persisted data/metadata.
    void Reset();
    const std::vector<LogRecord>& GetRecords() const { return records_; }
    lsn_t GetNextLsn() const;
    std::optional<lsn_t> GetPersistentLsn() const;
    void EnsureNextLsn(lsn_t minimum);
    const std::filesystem::path& GetPath() const { return path_; }

private:
    std::filesystem::path path_;
    std::filesystem::path sequence_path_;
    std::filesystem::path checkpoint_path_;
    std::ofstream output_;
    std::vector<LogRecord> records_;
    lsn_t next_lsn_ = 0;
    std::optional<lsn_t> persistent_lsn_;
    std::map<transaction_id_t, lsn_t> transaction_last_lsns_;
    mutable std::recursive_mutex mutex_;
    void PersistSequenceFloor();
};

}  // namespace udb
