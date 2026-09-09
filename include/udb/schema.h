#pragma once

#include "udb/column.h"

#include <limits>
#include <unordered_set>
#include <vector>

namespace udb {

class Schema {
public:
    // Empty schemas are valid; column names must be unique (case-sensitive).
    explicit Schema(std::vector<Column> columns) : columns_(std::move(columns)) {
        if (columns_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Too many schema columns");
        }
        std::unordered_set<std::string> names;
        for (const auto& column : columns_) {
            if (!names.insert(column.GetName()).second) {
                throw std::invalid_argument("Duplicate column name");
            }
        }
    }
    std::size_t GetColumnCount() const { return columns_.size(); }
    const Column& GetColumn(std::size_t index) const { return columns_.at(index); }
    const std::vector<Column>& GetColumns() const { return columns_; }

private:
    std::vector<Column> columns_;
};

}  // namespace udb
