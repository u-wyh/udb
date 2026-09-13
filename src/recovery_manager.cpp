#include "udb/recovery_manager.h"

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

void Redo(DiskManager& disk, const LogRecord& record,
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
        case LogRecordType::Begin:
        case LogRecordType::Commit:
        case LogRecordType::Abort: break;
    }
}

}  // namespace

std::map<page_id_t, bool> RecoveryManager::Recover(
    DiskManager& disk, LogManager& log_manager) {
    const auto records = log_manager.GetRecords();
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
        }
    }

    std::map<page_id_t, bool> page_states;
    // Transactions are single-threaded in this stage, so applying each unit in
    // WAL order preserves page reuse across committed and aborted units.
    for (const auto& unit : units) {
        if (unit.state == UnitState::Committed) {
            for (const auto index : unit.records) { Redo(disk, records[index], page_states); }
        } else {
            for (auto position = unit.records.size(); position > 0; --position) {
                Undo(disk, records[unit.records[position - 1]], page_states);
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
    return page_states;
}

}  // namespace udb
