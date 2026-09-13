#pragma once

#include "udb/transaction.h"

#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>

namespace udb {

enum class LockMode { Shared, Exclusive };

// Blocking table/row S/X lock manager. Requests are granted in FIFO order;
// one S-to-X upgrader is permitted per resource. Transaction end integration
// is intentionally handled by the next strict-2PL stage.
class LockManager {
public:
    void LockTable(Transaction& transaction, LockMode mode, table_id_t table_id);
    void UnlockTable(Transaction& transaction, table_id_t table_id);
    void LockRow(Transaction& transaction, LockMode mode, table_id_t table_id, RID rid);
    void UnlockRow(Transaction& transaction, table_id_t table_id, RID rid);
    void UnlockAll(Transaction& transaction);

private:
    struct Request {
        Transaction* transaction;
        LockMode mode;
        bool granted = false;
        bool upgrading = false;
    };
    struct Queue {
        std::list<Request> requests;
        Transaction* upgrader = nullptr;
        std::condition_variable condition;
    };

    using RequestIterator = std::list<Request>::iterator;
    static bool Compatible(LockMode left, LockMode right);
    static RequestIterator Find(Queue& queue, Transaction& transaction);
    static bool CanGrant(const Queue& queue, std::list<Request>::const_iterator request);
    static bool CanUpgrade(const Queue& queue, const Transaction& transaction);
    void LockTableLocked(std::unique_lock<std::mutex>& lock, Transaction& transaction,
                         LockMode mode, table_id_t table_id, const std::shared_ptr<Queue>& queue);
    void LockRowLocked(std::unique_lock<std::mutex>& lock, Transaction& transaction,
                       LockMode mode, RowLockId resource, const std::shared_ptr<Queue>& queue);

    std::mutex mutex_;
    std::map<table_id_t, std::shared_ptr<Queue>> table_queues_;
    std::map<RowLockId, std::shared_ptr<Queue>> row_queues_;
};

}  // namespace udb
