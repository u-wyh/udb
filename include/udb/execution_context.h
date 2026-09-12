#pragma once

#include "udb/transaction.h"

namespace udb {

class ExecutionContext {
public:
    explicit ExecutionContext(Transaction& transaction) : transaction_(transaction) {}
    Transaction& GetTransaction() const { return transaction_; }

private:
    Transaction& transaction_;
};

}  // namespace udb
