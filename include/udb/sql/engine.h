#pragma once

#include "udb/sql/executor.h"
#include "udb/transaction.h"

#include <string_view>

namespace udb::sql {

// Catalog must outlive the engine. Executes exactly one SQL statement and
// preserves Database's explicit Flush/Close persistence semantics.
class SqlEngine {
public:
    explicit SqlEngine(Catalog& catalog)
        : catalog_(catalog), executor_(catalog),
          transaction_manager_(&catalog.GetBufferPoolManager(), catalog.GetLogManager(),
                               &catalog.GetLockManager()) {}

    ExecutionResult ExecuteSQL(std::string_view sql);
    ExecutionResult ExecuteSQL(std::string_view sql, ExecutionContext& context);
    bool HasActiveTransaction() const { return current_transaction_ != nullptr; }

private:
    Catalog& catalog_;
    Executor executor_;
    TransactionManager transaction_manager_;
    Transaction* current_transaction_ = nullptr;
};

}  // namespace udb::sql
