#include "udb/sql/operator.h"

namespace udb::sql {

std::vector<Tuple> ExecutionOperator::Execute() {
    Init();
    std::vector<Tuple> rows;
    while (auto row = Next()) { rows.push_back(std::move(*row)); }
    return rows;
}

std::optional<Tuple> TableScanOperator::Next() {
    RequireInitialized();
    if (ended_) { return std::nullopt; }
    const auto rid = started_ ? heap_.GetNextRID(*current_) : heap_.GetFirstRID();
    if (!rid) { ended_ = true; return std::nullopt; }
    auto row = Tuple::Deserialize(heap_.GetRecord(*rid), schema_);
    current_ = rid;
    started_ = true;
    return row;
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

std::optional<Tuple> FilterOperator::Next() {
    RequireInitialized();
    while (auto row = input_->Next()) {
        if (Matches(predicate_, *row)) { return row; }
    }
    return std::nullopt;
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

std::optional<Tuple> ProjectionOperator::Next() {
    RequireInitialized();
    const auto row = input_->Next();
    if (!row) { return std::nullopt; }
    std::vector<Value> values;
    values.reserve(output_.GetColumnCount());
    if (!expressions_.empty()) {
        for (const auto& expression : expressions_) { values.push_back(EvaluateExpression(*expression, *row)); }
    } else {
        for (const auto index : indexes_) { values.push_back(row->GetValue(index)); }
    }
    return Tuple(output_, std::move(values));
}

LimitOperator::LimitOperator(std::unique_ptr<ExecutionOperator> input,
                            std::optional<std::size_t> limit, std::size_t offset)
    : input_(std::move(input)), limit_(limit), offset_(offset) {
    if (!input_) { throw std::invalid_argument("Limit requires an input"); }
}

void LimitOperator::Init() {
    input_->Init();
    skipped_ = 0;
    emitted_ = 0;
    ended_ = false;
    initialized_ = true;
}

std::optional<Tuple> LimitOperator::Next() {
    RequireInitialized();
    if (ended_ || (limit_ && emitted_ >= *limit_)) { return std::nullopt; }
    while (skipped_ < offset_) {
        if (!input_->Next()) { ended_ = true; return std::nullopt; }
        ++skipped_;
    }
    auto row = input_->Next();
    if (!row) { ended_ = true; return std::nullopt; }
    ++emitted_;
    return row;
}

}  // namespace udb::sql
