#pragma once

#include "udb/disk_manager.h"
#include "udb/log_manager.h"

#include <map>
#include <limits>
#include <optional>
#include <set>

namespace udb {

enum class RecoveryTransactionState { Running, Committed, Aborted };

struct RecoveryTransactionEntry {
    transaction_id_t transaction_id;
    RecoveryTransactionState state;
    lsn_t last_lsn;
    std::optional<timestamp_t> commit_timestamp;
};

struct RecoveryAnalysis {
    std::map<transaction_id_t, RecoveryTransactionEntry> transaction_table;
    std::map<page_id_t, lsn_t> dirty_page_table;
    std::set<transaction_id_t> winners;
    std::set<transaction_id_t> losers;
    std::set<transaction_id_t> aborted;
    std::optional<lsn_t> redo_start_lsn;
    timestamp_t maximum_commit_timestamp = 0;
};

struct RecoveryRedoResult {
    std::size_t examined = 0;
    std::size_t redone = 0;
    std::size_t skipped_by_dpt = 0;
    std::size_t skipped_by_page_lsn = 0;
    std::map<page_id_t, bool> page_states;
};

struct RecoveryUndoResult {
    std::size_t undone = 0;
    std::size_t compensation_records = 0;
    std::size_t completed_transactions = 0;
    bool complete = true;
    std::map<page_id_t, bool> page_states;
};

class RecoveryManager {
public:
    static RecoveryAnalysis Analyze(const LogManager& log_manager);
    static RecoveryRedoResult Redo(DiskManager& disk, const LogManager& log_manager,
                                   const RecoveryAnalysis& analysis);
    static RecoveryUndoResult Undo(DiskManager& disk, LogManager& log_manager,
                                   const RecoveryAnalysis& analysis,
                                   std::size_t maximum_actions =
                                       std::numeric_limits<std::size_t>::max());
    // Repeats history and reverses incomplete transactions with durable CLRs.
    static std::map<page_id_t, bool> Recover(
        DiskManager& disk, LogManager& log_manager);
};

}  // namespace udb
