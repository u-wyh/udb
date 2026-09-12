#include "udb/sql/operator.h"

namespace udb::sql {

std::vector<Tuple> TableScanOperator::Execute() {
    std::vector<Tuple> rows;
    for (auto rid = heap_.GetFirstRID(); rid; rid = heap_.GetNextRID(*rid)) {
        rows.push_back(Tuple::Deserialize(heap_.GetRecord(*rid), schema_));
    }
    return rows;
}

FilterOperator::FilterOperator(std::unique_ptr<ExecutionOperator> input, BoundExpressionPtr predicate)
    : input_(std::move(input)), predicate_(std::move(predicate)) {
    if (!input_) { throw std::invalid_argument("Filter requires an input"); }
    if (predicate_ && predicate_->type != TypeId::BOOLEAN) {
        throw std::invalid_argument("Filter predicate must be BOOLEAN");
    }
}

bool FilterOperator::Matches(const BoundExpressionPtr& predicate, const Tuple& tuple) {
    if (!predicate) { return true; }
    const auto value = EvaluateExpression(*predicate, tuple);
    if (value.GetType() != TypeId::BOOLEAN) {
        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
    }
    return !value.IsNull() && value.GetBoolean();
}

std::vector<Tuple> FilterOperator::Execute() {
    auto rows = input_->Execute();
    std::vector<Tuple> result;
    for (auto& row : rows) {
        if (Matches(predicate_, row)) { result.push_back(std::move(row)); }
    }
    return result;
}

ProjectionOperator::ProjectionOperator(std::unique_ptr<ExecutionOperator> input, Schema output,
    std::vector<std::size_t> indexes, std::vector<BoundExpressionPtr> expressions)
    : input_(std::move(input)), output_(std::move(output)), indexes_(std::move(indexes)),
      expressions_(std::move(expressions)) {
    if (!input_) { throw std::invalid_argument("Projection requires an input"); }
    if ((!expressions_.empty() && !indexes_.empty()) ||
        (expressions_.empty() ? indexes_.size() : expressions_.size()) != output_.GetColumnCount()) {
        throw std::invalid_argument("Projection does not match output schema");
    }
    for (const auto& expression : expressions_) {
        if (!expression) { throw std::invalid_argument("Projection expression is missing"); }
    }
}

std::vector<Tuple> ProjectionOperator::Execute() {
    auto rows = input_->Execute();
    std::vector<Tuple> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<Value> values;
        values.reserve(output_.GetColumnCount());
        if (!expressions_.empty()) {
            for (const auto& expression : expressions_) { values.push_back(EvaluateExpression(*expression, row)); }
        } else {
            for (const auto index : indexes_) { values.push_back(row.GetValue(index)); }
        }
        result.emplace_back(output_, std::move(values));
    }
    return result;
}

}  // namespace udb::sql
