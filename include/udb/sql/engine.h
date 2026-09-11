#pragma once

#include "udb/sql/executor.h"

#include <string_view>

namespace udb::sql {

// Catalog must outlive the engine. Executes exactly one SQL statement and
// preserves Database's explicit Flush/Close persistence semantics.
class SqlEngine {
public:
    explicit SqlEngine(Catalog& catalog) : catalog_(catalog), executor_(catalog) {}

    ExecutionResult ExecuteSQL(std::string_view sql);

private:
    Catalog& catalog_;
    Executor executor_;
};

}  // namespace udb::sql
