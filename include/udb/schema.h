#pragma once

#include "udb/column.h"

#include <limits>
#include <unordered_set>
#include <vector>

namespace udb {

struct ForeignKeyConstraint {
    std::vector<std::size_t> column_indexes;
    std::uint64_t referenced_table_id;
    std::vector<std::size_t> referenced_column_indexes;

    friend bool operator==(const ForeignKeyConstraint& a, const ForeignKeyConstraint& b) {
        return a.column_indexes == b.column_indexes &&
               a.referenced_table_id == b.referenced_table_id &&
               a.referenced_column_indexes == b.referenced_column_indexes;
    }
};

class Schema {
public:
    // Empty schemas are valid; column names must be unique (case-sensitive).
    explicit Schema(std::vector<Column> columns,
                    std::vector<std::string> check_expressions = {},
                    std::vector<ForeignKeyConstraint> foreign_keys = {})
        : columns_(std::move(columns)), check_expressions_(std::move(check_expressions)),
          foreign_keys_(std::move(foreign_keys)) {
        if (columns_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Too many schema columns");
        }
        for (const auto& check : check_expressions_) {
            if (check.empty()) { throw std::invalid_argument("Empty CHECK expression"); }
        }
        for (const auto& foreign_key : foreign_keys_) {
            if (foreign_key.column_indexes.empty() ||
                foreign_key.column_indexes.size() != foreign_key.referenced_column_indexes.size()) {
                throw std::invalid_argument("Invalid FOREIGN KEY column list");
            }
            for (const auto column : foreign_key.column_indexes) {
                if (column >= columns_.size()) { throw std::invalid_argument("Invalid FOREIGN KEY column"); }
            }
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
    const std::vector<ForeignKeyConstraint>& GetForeignKeys() const { return foreign_keys_; }

private:
    std::vector<Column> columns_;
    std::vector<std::string> check_expressions_;
    std::vector<ForeignKeyConstraint> foreign_keys_;
};

}  // namespace udb
