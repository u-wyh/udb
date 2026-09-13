#pragma once

#include "udb/sql/executor.h"
#include "udb/transaction.h"

#include <string_view>

namespace udb::sql {

// Catalog must outlive the engine. Executes exactly one SQL statement and
// preserves Database's explicit Flush/Close persistence semantics.
class SqlEngine {
public:
    explicit SqlEngine(Catalog& catalog,
                       IsolationLevel default_isolation = IsolationLevel::RepeatableRead)
        : catalog_(catalog), executor_(catalog),
          transaction_manager_(catalog.GetTransactionManager()),
          default_isolation_(default_isolation) {}

    ExecutionResult ExecuteSQL(std::string_view sql);
    ExecutionResult ExecuteSQL(std::string_view sql, ExecutionContext& context);
    bool HasActiveTransaction() const { return current_transaction_ != nullptr; }
    IsolationLevel GetDefaultIsolationLevel() const { return default_isolation_; }
    void SetDefaultIsolationLevel(IsolationLevel isolation_level);

private:
    Catalog& catalog_;
    Executor executor_;
    TransactionManager& transaction_manager_;
    IsolationLevel default_isolation_;
    Transaction* current_transaction_ = nullptr;
};

}  // namespace udb::sql
