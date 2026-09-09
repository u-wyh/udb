#pragma once

#include "udb/type_id.h"

#include <string>
#include <variant>
#include <vector>

namespace udb::sql {

struct ColumnDefinition {
    std::string name;
    TypeId type;
    std::uint32_t max_length = 0;
};

struct CreateTableStatement {
    std::string table_name;
    std::vector<ColumnDefinition> columns;
};

// NULL is untyped until binding. Integers use signed 64-bit literal range;
// schema-specific narrowing belongs to the future binder.
using Literal = std::variant<std::monostate, std::int64_t, std::string, bool>;

struct InsertStatement {
    std::string table_name;
    std::vector<Literal> values;
};

struct SelectStatement {
    std::string table_name;
    bool select_all = false;
    std::vector<std::string> column_names;
};

using Statement = std::variant<CreateTableStatement, InsertStatement, SelectStatement>;

}  // namespace udb::sql
