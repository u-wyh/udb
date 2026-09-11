#pragma once

#include "udb/table_metadata.h"
#include "udb/sql/bound_expression.h"
#include "udb/value.h"

#include <variant>

namespace udb::sql {

// Bound statements own schema/value snapshots, not references to AST or Catalog.
struct BoundCreateTableStatement {
    std::string table_name;
    Schema schema;
};

struct BoundInsertStatement {
    table_id_t table_id;
    std::string table_name;
    Schema schema;
    std::vector<Value> values;
};

struct BoundSelectStatement {
    table_id_t table_id;
    std::string table_name;
    std::vector<std::size_t> column_indexes;
    Schema output_schema;
    BoundExpressionPtr predicate = nullptr;
};

struct BoundDeleteStatement {
    table_id_t table_id;
    std::string table_name;
    Schema schema;
    BoundExpressionPtr predicate = nullptr;
};

struct BoundUpdateAssignment {
    std::size_t column_index;
    Value value;
};

struct BoundUpdateStatement {
    table_id_t table_id;
    std::string table_name;
    Schema schema;
    std::vector<BoundUpdateAssignment> assignments;
    BoundExpressionPtr predicate = nullptr;
};

using BoundStatement = std::variant<BoundCreateTableStatement, BoundInsertStatement,
                                    BoundSelectStatement, BoundDeleteStatement, BoundUpdateStatement>;

}  // namespace udb::sql
