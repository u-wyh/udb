#pragma once

#include "udb/column.h"

#include <limits>
#include <unordered_set>
#include <vector>

namespace udb {

class Schema {
public:
    // Empty schemas are valid; column names must be unique (case-sensitive).
    explicit Schema(std::vector<Column> columns,
                    std::vector<std::string> check_expressions = {})
        : columns_(std::move(columns)), check_expressions_(std::move(check_expressions)) {
        if (columns_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Too many schema columns");
        }
        for (const auto& check : check_expressions_) {
            if (check.empty()) { throw std::invalid_argument("Empty CHECK expression"); }
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
    const std::vector<std::string>& GetCheckExpressions() const { return check_expressions_; }

private:
    std::vector<Column> columns_;
    std::vector<std::string> check_expressions_;
};

}  // namespace udb
