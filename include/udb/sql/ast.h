#pragma once

#include "udb/type_id.h"
#include "udb/sql/aggregate.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace udb::sql {

struct DefaultLiteral {};

// NULL is untyped until binding. Integers use signed 64-bit literal range;
// schema-specific narrowing belongs to binding. DEFAULT is only valid in
// INSERT values, UPDATE assignments, and column definitions.
using Literal = std::variant<std::monostate, DefaultLiteral, std::int64_t, std::string, bool>;

struct ColumnDefinition {
    ColumnDefinition() = default;
    ColumnDefinition(std::string column_name, TypeId column_type,
                     std::uint32_t length = 0, bool required = false,
                     std::optional<Literal> default_literal = std::nullopt)
        : name(std::move(column_name)), type(column_type), max_length(length),
          not_null(required), default_value(std::move(default_literal)) {}
    std::string name;
    TypeId type = TypeId::INTEGER;
    std::uint32_t max_length = 0;
    bool not_null = false;
    std::optional<Literal> default_value;
};

struct CreateTableStatement {
    std::string table_name;
    std::vector<ColumnDefinition> columns;
};

struct CreateIndexStatement {
    std::string index_name;
    std::string table_name;
    std::string column_name;
    std::vector<std::string> column_names = {};
};

struct DropTableStatement {
    std::string table_name;
};

struct DropIndexStatement {
    std::string index_name;
};

enum class ComparisonOperator { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
enum class LogicalOperator { And, Or, Not };
enum class ArithmeticOperator { Add, Subtract, Multiply, Divide };

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
struct ArithmeticExpression {
    ArithmeticOperator op;
    ExpressionPtr left;
    ExpressionPtr right;
};

struct Expression {
    using Node = std::variant<ColumnExpression, LiteralExpression, ComparisonExpression,
                              LogicalExpression, ArithmeticExpression>;
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

struct SelectExpression {
    ExpressionPtr expression;
    std::optional<std::string> alias;
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
    std::optional<std::string> group_by;
    ExpressionPtr having = nullptr;
    std::vector<SelectExpression> projections;
    std::optional<std::string> joined_table;
    ExpressionPtr join_condition = nullptr;
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
