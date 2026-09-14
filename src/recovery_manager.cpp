#include "udb/recovery_manager.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace udb {
namespace {

enum class UnitState { Active, Committed, Aborted };

struct Unit {
    transaction_id_t id;
    UnitState state = UnitState::Active;
    std::vector<std::size_t> records;
};

void LegacyRedo(DiskManager& disk, const LogRecord& record,
                std::map<page_id_t, bool>& states) {
    switch (record.GetType()) {
        case LogRecordType::PageWrite:
            disk.RecoveryWritePage(*record.GetPageId(), *record.GetAfterImage());
            break;
        case LogRecordType::PageAllocate:
            disk.RecoveryWritePage(*record.GetPageId(), Page{});
            states[*record.GetPageId()] = true;
            break;
        case LogRecordType::PageFree:
            states[*record.GetPageId()] = false;
            break;
        case LogRecordType::Compensation:
            disk.RecoveryWritePage(*record.GetPageId(), *record.GetAfterImage());
            if (*record.GetCompensationType() == CompensationType::PageAllocate) {
                states[*record.GetPageId()] = false;
            } else if (*record.GetCompensationType() == CompensationType::PageFree) {
                states[*record.GetPageId()] = true;
            }
            break;
        case LogRecordType::Begin:
        case LogRecordType::Commit:
        case LogRecordType::Abort: break;
    }
}

void Undo(DiskManager& disk, const LogRecord& record,
          std::map<page_id_t, bool>& states) {
    switch (record.GetType()) {
        case LogRecordType::PageWrite:
            disk.RecoveryWritePage(*record.GetPageId(), *record.GetBeforeImage());
            break;
        case LogRecordType::PageAllocate:
            states[*record.GetPageId()] = false;
            break;
        case LogRecordType::PageFree:
            disk.RecoveryWritePage(*record.GetPageId(), *record.GetBeforeImage());
            states[*record.GetPageId()] = true;
            break;
        case LogRecordType::Compensation:
            break;
        case LogRecordType::Begin:
        case LogRecordType::Commit:
        case LogRecordType::Abort: break;
    }
}

}  // namespace

RecoveryAnalysis RecoveryManager::Analyze(const LogManager& log_manager) {
    RecoveryAnalysis analysis;
    std::map<transaction_id_t, lsn_t> active_last_lsns;
    for (const auto& record : log_manager.GetRecords()) {
        const auto id = record.GetTransactionId();
        if (record.GetType() == LogRecordType::Begin) {
            if (active_last_lsns.count(id) != 0) {
                throw std::runtime_error("Nested WAL transaction ID during Analysis");
            }
            active_last_lsns[id] = record.GetLsn();
            analysis.transaction_table.insert_or_assign(
                id, RecoveryTransactionEntry{id, RecoveryTransactionState::Running,
                                             record.GetLsn(), std::nullopt});
            continue;
        }
        const auto active = active_last_lsns.find(id);
        if (active == active_last_lsns.end()) {
            throw std::runtime_error("WAL record has no active transaction during Analysis");
        }
        if (record.GetPrevLsn() != std::optional<lsn_t>(active->second)) {
            throw std::runtime_error("Broken WAL transaction chain during Analysis");
        }
        active->second = record.GetLsn();
        auto& transaction = analysis.transaction_table.at(id);
        transaction.last_lsn = record.GetLsn();
        if (record.GetPageId()) {
            analysis.dirty_page_table.emplace(*record.GetPageId(), record.GetLsn());
        }
        if (record.GetType() == LogRecordType::Commit) {
            transaction.state = RecoveryTransactionState::Committed;
            transaction.commit_timestamp = record.GetCommitTimestamp();
            analysis.winners.insert(id);
            if (record.GetCommitTimestamp()) {
                analysis.maximum_commit_timestamp = std::max(
                    analysis.maximum_commit_timestamp, *record.GetCommitTimestamp());
            }
            active_last_lsns.erase(active);
        } else if (record.GetType() == LogRecordType::Abort) {
            transaction.state = RecoveryTransactionState::Aborted;
            analysis.aborted.insert(id);
            active_last_lsns.erase(active);
        }
    }
    for (const auto& [id, last_lsn] : active_last_lsns) {
        static_cast<void>(last_lsn);
        analysis.losers.insert(id);
    }
    if (!analysis.dirty_page_table.empty()) {
        analysis.redo_start_lsn = std::min_element(
            analysis.dirty_page_table.begin(), analysis.dirty_page_table.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; })
                                      ->second;
    }
    return analysis;
}

RecoveryRedoResult RecoveryManager::Redo(DiskManager& disk,
                                         const LogManager& log_manager,
                                         const RecoveryAnalysis& analysis) {
    RecoveryRedoResult result;
    if (!analysis.redo_start_lsn) { return result; }
    const auto& records = log_manager.GetRecords();
    for (const auto& record : records) {
        if (record.GetLsn() < *analysis.redo_start_lsn || !record.GetPageId()) { continue; }
        ++result.examined;
        const auto page_id = *record.GetPageId();
        const auto dirty = analysis.dirty_page_table.find(page_id);
        if (dirty == analysis.dirty_page_table.end() || record.GetLsn() < dirty->second) {
            ++result.skipped_by_dpt;
            continue;
        }
        std::optional<lsn_t> page_lsn;
        if (page_id >= 0 && page_id < disk.GetPageCount() && disk.IsPageAllocated(page_id)) {
            page_lsn = disk.GetPageLsn(page_id);
        }
        if (page_lsn && *page_lsn >= record.GetLsn()) {
            ++result.skipped_by_page_lsn;
            continue;
        }
        switch (record.GetType()) {
            case LogRecordType::PageWrite:
                disk.RecoveryWritePage(page_id, *record.GetAfterImage(), record.GetLsn());
                break;
            case LogRecordType::PageAllocate:
                disk.RecoveryWritePage(page_id, Page{}, record.GetLsn());
                result.page_states[page_id] = true;
                break;
            case LogRecordType::PageFree:
                if (page_id >= disk.GetPageCount()) {
                    throw std::runtime_error("Redo PAGE_FREE references a missing page");
                }
                disk.SetPageLsn(page_id, record.GetLsn());
                result.page_states[page_id] = false;
                break;
            case LogRecordType::Compensation:
                disk.RecoveryWritePage(page_id, *record.GetAfterImage(), record.GetLsn());
                if (*record.GetCompensationType() == CompensationType::PageAllocate) {
                    result.page_states[page_id] = false;
                } else if (*record.GetCompensationType() == CompensationType::PageFree) {
                    result.page_states[page_id] = true;
                }
                break;
            case LogRecordType::Begin:
            case LogRecordType::Commit:
            case LogRecordType::Abort:
                throw std::logic_error("Analysis DPT contains a non-page WAL record");
        }
        ++result.redone;
    }
    return result;
}

std::map<page_id_t, bool> RecoveryManager::Recover(
    DiskManager& disk, LogManager& log_manager) {
    const auto records = log_manager.GetRecords();
    timestamp_t recovered_commit_timestamp = 0;
    std::vector<Unit> units;
    std::unordered_map<transaction_id_t, std::size_t> active;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        const auto id = record.GetTransactionId();
        if (record.GetType() == LogRecordType::Begin) {
            if (active.count(id) != 0) { throw std::runtime_error("Nested WAL transaction ID"); }
            units.push_back(Unit{id, UnitState::Active, {}});
            active[id] = units.size() - 1;
        }
        const auto found = active.find(id);
        if (found == active.end()) { throw std::runtime_error("WAL record has no active transaction"); }
        units[found->second].records.push_back(i);
        if (record.GetType() == LogRecordType::Commit ||
            record.GetType() == LogRecordType::Abort) {
            units[found->second].state = record.GetType() == LogRecordType::Commit
                                                ? UnitState::Committed : UnitState::Aborted;
            active.erase(found);
            if (record.GetType() == LogRecordType::Commit && record.GetCommitTimestamp()) {
                recovered_commit_timestamp = std::max(recovered_commit_timestamp,
                                                      *record.GetCommitTimestamp());
            }
        }
    }

    std::map<page_id_t, bool> page_states;
    // Strict 2PL serializes conflicting page writers by transaction end, which
    // can differ from BEGIN order when sessions run concurrently. Replay ended
    // units by their terminal WAL position; active losers follow all ended work.
    std::vector<const Unit*> replay_order;
    replay_order.reserve(units.size());
    for (const auto& unit : units) { replay_order.push_back(&unit); }
    std::stable_sort(replay_order.begin(), replay_order.end(),
                     [](const Unit* left, const Unit* right) {
                         const bool left_active = left->state == UnitState::Active;
                         const bool right_active = right->state == UnitState::Active;
                         if (left_active != right_active) { return !left_active; }
                         return left->records.back() < right->records.back();
                     });
    for (const auto* unit : replay_order) {
        if (unit->state == UnitState::Committed) {
            for (const auto index : unit->records) { LegacyRedo(disk, records[index], page_states); }
        } else {
            for (auto position = unit->records.size(); position > 0; --position) {
                Undo(disk, records[unit->records[position - 1]], page_states);
            }
        }
    }
    bool appended_abort = false;
    for (const auto& unit : units) {
        if (unit.state == UnitState::Active) {
            log_manager.Append(LogRecord::Abort(unit.id));
            appended_abort = true;
        }
    }
    if (appended_abort) { log_manager.Flush(); }
    TransactionManager::RestoreLastCommitTimestamp(recovered_commit_timestamp);
    return page_states;
}

}  // namespace udb
