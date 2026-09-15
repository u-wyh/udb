#include "udb/recovery_manager.h"

#include <algorithm>
#include <stdexcept>
#include <queue>

namespace udb {
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
        if (record.GetType() == LogRecordType::PageAllocate) {
            result.page_states[page_id] = true;
        } else if (record.GetType() == LogRecordType::PageFree) {
            result.page_states[page_id] = false;
        } else if (record.GetType() == LogRecordType::Compensation) {
            if (*record.GetCompensationType() == CompensationType::PageAllocate) {
                result.page_states[page_id] = false;
            } else if (*record.GetCompensationType() == CompensationType::PageFree) {
                result.page_states[page_id] = true;
            }
        }
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
                break;
            case LogRecordType::PageFree:
                if (page_id >= disk.GetPageCount()) {
                    throw std::runtime_error("Redo PAGE_FREE references a missing page");
                }
                // Recovery may start from metadata that already marks this page
                // free. Install a physical image so its pageLSN can still advance;
                // the allocation state is applied after all recovery passes.
                disk.RecoveryWritePage(page_id, Page{}, record.GetLsn());
                break;
            case LogRecordType::Compensation:
                disk.RecoveryWritePage(page_id, *record.GetAfterImage(), record.GetLsn());
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

RecoveryUndoResult RecoveryManager::Undo(DiskManager& disk, LogManager& log_manager,
                                         const RecoveryAnalysis& analysis,
                                         std::size_t maximum_actions) {
    RecoveryUndoResult result;
    std::map<lsn_t, LogRecord> records;
    for (const auto& record : log_manager.GetRecords()) {
        records.emplace(record.GetLsn(), record);
    }
    using Work = std::pair<lsn_t, transaction_id_t>;
    std::priority_queue<Work> work;
    for (const auto id : analysis.losers) {
        work.emplace(analysis.transaction_table.at(id).last_lsn, id);
    }
    while (!work.empty()) {
        if (result.undone == maximum_actions) {
            result.complete = false;
            return result;
        }
        const auto [lsn, transaction_id] = work.top();
        work.pop();
        const auto found = records.find(lsn);
        if (found == records.end() || found->second.GetTransactionId() != transaction_id) {
            throw std::runtime_error("Broken WAL transaction chain during Undo");
        }
        const auto record = found->second;
        std::optional<lsn_t> next = record.GetPrevLsn();
        if (record.GetType() == LogRecordType::Compensation) {
            next = record.GetUndoNextLsn();
            if (!next) {
                log_manager.Append(LogRecord::Abort(transaction_id));
                log_manager.Flush();
                ++result.completed_transactions;
            }
        } else if (record.GetType() == LogRecordType::PageWrite ||
                   record.GetType() == LogRecordType::PageAllocate ||
                   record.GetType() == LogRecordType::PageFree) {
            const auto page_id = *record.GetPageId();
            Page image;
            CompensationType action;
            if (record.GetType() == LogRecordType::PageWrite) {
                image = *record.GetBeforeImage();
                action = CompensationType::PageWrite;
            } else if (record.GetType() == LogRecordType::PageAllocate) {
                image = Page{};
                action = CompensationType::PageAllocate;
            } else {
                image = *record.GetBeforeImage();
                action = CompensationType::PageFree;
            }
            const auto clr_lsn = log_manager.Append(LogRecord::Compensation(
                transaction_id, page_id, action, image, next));
            log_manager.Flush();
            disk.RecoveryWritePage(page_id, image, clr_lsn);
            if (action == CompensationType::PageAllocate) {
                result.page_states[page_id] = false;
            } else if (action == CompensationType::PageFree) {
                result.page_states[page_id] = true;
            }
            ++result.undone;
            ++result.compensation_records;
        } else if (record.GetType() == LogRecordType::Begin) {
            log_manager.Append(LogRecord::Abort(transaction_id));
            log_manager.Flush();
            ++result.completed_transactions;
            next.reset();
        } else {
            throw std::runtime_error("Undo encountered a completed transaction record");
        }
        if (next) { work.emplace(*next, transaction_id); }
    }
    return result;
}

std::map<page_id_t, bool> RecoveryManager::Recover(
    DiskManager& disk, LogManager& log_manager) {
    const auto analysis = Analyze(log_manager);
    auto redo = Redo(disk, log_manager, analysis);
    auto undo = Undo(disk, log_manager, analysis);
    for (const auto& [page_id, allocated] : undo.page_states) {
        redo.page_states.insert_or_assign(page_id, allocated);
    }
    TransactionManager::RestoreLastCommitTimestamp(analysis.maximum_commit_timestamp);
    return redo.page_states;
}

}  // namespace udb
