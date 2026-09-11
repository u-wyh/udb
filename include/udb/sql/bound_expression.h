#pragma once

#include "udb/sql/ast.h"
#include "udb/tuple.h"

#include <memory>
#include <variant>

namespace udb::sql {

struct BoundExpression;
using BoundExpressionPtr = std::shared_ptr<const BoundExpression>;

struct BoundColumnExpression {
    std::size_t column_index;
};
struct BoundLiteralExpression {
    Value value;
};
struct BoundComparisonExpression {
    ComparisonOperator op;
    BoundExpressionPtr left;
    BoundExpressionPtr right;
};
struct BoundLogicalExpression {
    LogicalOperator op;
    BoundExpressionPtr left;
    BoundExpressionPtr right;
};
struct BoundArithmeticExpression {
    ArithmeticOperator op;
    BoundExpressionPtr left;
    BoundExpressionPtr right;
};

struct BoundExpression {
    using Node = std::variant<BoundColumnExpression, BoundLiteralExpression,
                              BoundComparisonExpression, BoundLogicalExpression,
                              BoundArithmeticExpression>;
    BoundExpression(TypeId result_type, Node value) : type(result_type), node(std::move(value)) {}
    TypeId type;
    Node node;
};

// Evaluates an already bound expression. Column references use tuple indexes;
// comparisons and logical operations return BOOLEAN, possibly NULL.
Value EvaluateExpression(const BoundExpression& expression, const Tuple& tuple);

}  // namespace udb::sql
