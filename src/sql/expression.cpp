#include "udb/sql/bound_expression.h"

namespace udb::sql {
namespace {

int Compare(const Value& left, const Value& right) {
    if (left.GetType() != right.GetType() || left.IsNull() || right.IsNull()) {
        throw std::invalid_argument("Cannot compare values with different types or NULL directly");
    }
    switch (left.GetType()) {
        case TypeId::BOOLEAN:
            return static_cast<int>(left.GetBoolean()) - static_cast<int>(right.GetBoolean());
        case TypeId::INTEGER:
            return left.GetInteger() < right.GetInteger() ? -1 : left.GetInteger() > right.GetInteger() ? 1 : 0;
        case TypeId::BIGINT:
            return left.GetBigInt() < right.GetBigInt() ? -1 : left.GetBigInt() > right.GetBigInt() ? 1 : 0;
        case TypeId::VARCHAR:
            return left.GetVarchar() < right.GetVarchar() ? -1 : left.GetVarchar() > right.GetVarchar() ? 1 : 0;
    }
    throw std::invalid_argument("Unknown comparison type");
}

bool ApplyComparison(ComparisonOperator op, int order) {
    switch (op) {
        case ComparisonOperator::Equal: return order == 0;
        case ComparisonOperator::NotEqual: return order != 0;
        case ComparisonOperator::Less: return order < 0;
        case ComparisonOperator::LessEqual: return order <= 0;
        case ComparisonOperator::Greater: return order > 0;
        case ComparisonOperator::GreaterEqual: return order >= 0;
    }
    throw std::invalid_argument("Unknown comparison operator");
}

Value Evaluate(const BoundExpressionPtr& expression, const Tuple& tuple) {
    if (!expression) { throw std::invalid_argument("Missing bound expression"); }
    return EvaluateExpression(*expression, tuple);
}

}  // namespace

Value EvaluateExpression(const BoundExpression& expression, const Tuple& tuple) {
    if (const auto* column = std::get_if<BoundColumnExpression>(&expression.node)) {
        const auto value = tuple.GetValue(column->column_index);
        if (value.GetType() != expression.type) { throw std::invalid_argument("Bound column type mismatch"); }
        return value;
    }
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression.node)) {
        if (literal->value.GetType() != expression.type) { throw std::invalid_argument("Bound literal type mismatch"); }
        return literal->value;
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.node)) {
        const auto left = Evaluate(comparison->left, tuple);
        const auto right = Evaluate(comparison->right, tuple);
        if (expression.type != TypeId::BOOLEAN || left.GetType() != right.GetType()) {
            throw std::invalid_argument("Invalid bound comparison");
        }
        if (left.IsNull() || right.IsNull()) { return Value::Null(TypeId::BOOLEAN); }
        return Value::Boolean(ApplyComparison(comparison->op, Compare(left, right)));
    }
    const auto& logical = std::get<BoundLogicalExpression>(expression.node);
    if (expression.type != TypeId::BOOLEAN) { throw std::invalid_argument("Invalid bound logical expression"); }
    const auto left = Evaluate(logical.left, tuple);
    if (left.GetType() != TypeId::BOOLEAN) { throw std::invalid_argument("Logical operand must be BOOLEAN"); }
    if (logical.op == LogicalOperator::Not) {
        if (logical.right) { throw std::invalid_argument("NOT accepts one operand"); }
        return left.IsNull() ? Value::Null(TypeId::BOOLEAN) : Value::Boolean(!left.GetBoolean());
    }
    if (logical.op == LogicalOperator::And && !left.IsNull() && !left.GetBoolean()) { return Value::Boolean(false); }
    if (logical.op == LogicalOperator::Or && !left.IsNull() && left.GetBoolean()) { return Value::Boolean(true); }
    const auto right = Evaluate(logical.right, tuple);
    if (right.GetType() != TypeId::BOOLEAN) { throw std::invalid_argument("Logical operand must be BOOLEAN"); }
    if (logical.op == LogicalOperator::And) {
        if (!right.IsNull() && !right.GetBoolean()) { return Value::Boolean(false); }
        return left.IsNull() || right.IsNull() ? Value::Null(TypeId::BOOLEAN) : Value::Boolean(true);
    }
    if (logical.op == LogicalOperator::Or) {
        if (!right.IsNull() && right.GetBoolean()) { return Value::Boolean(true); }
        return left.IsNull() || right.IsNull() ? Value::Null(TypeId::BOOLEAN) : Value::Boolean(false);
    }
    throw std::invalid_argument("Unknown logical operator");
}

}  // namespace udb::sql
