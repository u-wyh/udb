#pragma once

#include "udb/disk_manager.h"
#include "udb/log_manager.h"

#include <map>

namespace udb {

class RecoveryManager {
public:
    // Replays committed transactions and reverses aborted/incomplete transactions.
    static std::map<page_id_t, bool> Recover(
        DiskManager& disk, LogManager& log_manager);
};

}  // namespace udb
