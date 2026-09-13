#pragma once

#include "udb/transaction.h"
#include "udb/lock_manager.h"

namespace udb {

class ExecutionContext {
public:
    explicit ExecutionContext(Transaction& transaction) : transaction_(transaction) {}
    ExecutionContext(Transaction& transaction, LockManager& lock_manager)
        : transaction_(transaction), lock_manager_(&lock_manager) {}
    Transaction& GetTransaction() const { return transaction_; }
    LockManager* GetLockManager() const { return lock_manager_; }

private:
    Transaction& transaction_;
    LockManager* lock_manager_ = nullptr;
};

}  // namespace udb
