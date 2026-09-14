#pragma once

#include "udb/disk_manager.h"
#include "udb/log_manager.h"

#include <map>
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

class RecoveryManager {
public:
    static RecoveryAnalysis Analyze(const LogManager& log_manager);
    // Replays committed transactions and reverses aborted/incomplete transactions.
    static std::map<page_id_t, bool> Recover(
        DiskManager& disk, LogManager& log_manager);
};

}  // namespace udb
