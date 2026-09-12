#pragma once

#include "udb/value.h"

#include <optional>
#include <vector>

namespace udb {

struct ColumnStatistics {
    std::size_t null_count = 0;
    std::size_t non_null_count = 0;
    std::size_t distinct_count = 0;
    std::optional<Value> minimum;
    std::optional<Value> maximum;
};

// Statistics are an explicit in-memory snapshot produced by AnalyzeTable.
// They are intentionally not persisted or maintained incrementally yet.
struct TableStatistics {
    std::size_t row_count = 0;
    std::vector<ColumnStatistics> columns;
};

}  // namespace udb
