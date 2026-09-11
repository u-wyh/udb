#pragma once

#include "udb/type_id.h"
#include "udb/sql/aggregate.h"

#include <memory>
#include <optional>
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

struct CreateIndexStatement {
    std::string index_name;
    std::string table_name;
    std::string column_name;
};

struct DropTableStatement {
    std::string table_name;
};

struct DropIndexStatement {
    std::string index_name;
};

// NULL is untyped until binding. Integers use signed 64-bit literal range;
// schema-specific narrowing belongs to binding.
using Literal = std::variant<std::monostate, std::int64_t, std::string, bool>;

enum class ComparisonOperator { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
enum class LogicalOperator { And, Or, Not };

struct Expression;
using ExpressionPtr = std::shared_ptr<const Expression>;

struct ColumnExpression { std::string name; };
struct LiteralExpression { Literal value; };
struct ComparisonExpression {
    ComparisonOperator op;
    ExpressionPtr left;
    ExpressionPtr right;
};
struct LogicalExpression {
    LogicalOperator op;
    ExpressionPtr left;
    ExpressionPtr right;  // Empty only for NOT.
};

struct Expression {
    using Node = std::variant<ColumnExpression, LiteralExpression, ComparisonExpression, LogicalExpression>;
    explicit Expression(Node value) : node(std::move(value)) {}
    Node node;
};

struct InsertStatement {
    std::string table_name;
    std::vector<Literal> values;
};

struct OrderBy {
    std::string column_name;
    bool ascending = true;
};

struct AggregateExpression {
    AggregateType type;
    std::optional<std::string> column_name;  // Empty only for COUNT(*).
};

struct SelectStatement {
    std::string table_name;
    bool select_all = false;
    std::vector<std::string> column_names;
    ExpressionPtr predicate = nullptr;
    std::vector<OrderBy> order_by;
    std::optional<std::size_t> limit;
    std::size_t offset = 0;
    std::vector<AggregateExpression> aggregates;
};

struct DeleteStatement {
    std::string table_name;
    ExpressionPtr predicate = nullptr;
};

struct UpdateAssignment {
    std::string column_name;
    Literal value;
};

struct UpdateStatement {
    std::string table_name;
    std::vector<UpdateAssignment> assignments;
    ExpressionPtr predicate = nullptr;
};

using Statement = std::variant<CreateTableStatement, CreateIndexStatement, DropTableStatement,
                               DropIndexStatement, InsertStatement, SelectStatement,
                               DeleteStatement, UpdateStatement>;

}  // namespace udb::sql
