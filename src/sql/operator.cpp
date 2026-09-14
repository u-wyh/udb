#include "udb/sql/operator.h"

#include <algorithm>
#include <limits>

namespace udb::sql {
namespace {
bool ValueLess(const Value& left, const Value& right) {
    switch (left.GetType()) {
        case TypeId::BOOLEAN: return !left.GetBoolean() && right.GetBoolean();
        case TypeId::INTEGER: return left.GetInteger() < right.GetInteger();
        case TypeId::BIGINT: return left.GetBigInt() < right.GetBigInt();
        case TypeId::VARCHAR: return left.GetVarchar() < right.GetVarchar();
        case TypeId::DOUBLE: return left.GetDouble() < right.GetDouble();
    }
    throw std::invalid_argument("Unknown aggregate comparison type");
}

bool OrderLess(const Tuple& left, const Tuple& right,
               const std::vector<PlanOrderBy>& order_by) {
    for (const auto& order : order_by) {
        const auto& a = left.GetValue(order.column_index);
        const auto& b = right.GetValue(order.column_index);
        if (a.IsNull() || b.IsNull()) {
            if (a.IsNull() && b.IsNull()) { continue; }
            return !a.IsNull();  // NULLS LAST for every key and direction.
        }
        bool less = false;
        bool greater = false;
        switch (a.GetType()) {
            case TypeId::BOOLEAN:
                less = !a.GetBoolean() && b.GetBoolean();
                greater = a.GetBoolean() && !b.GetBoolean();
                break;
            case TypeId::INTEGER:
                less = a.GetInteger() < b.GetInteger();
                greater = a.GetInteger() > b.GetInteger();
                break;
            case TypeId::BIGINT:
                less = a.GetBigInt() < b.GetBigInt();
                greater = a.GetBigInt() > b.GetBigInt();
                break;
            case TypeId::VARCHAR:
                less = a.GetVarchar() < b.GetVarchar();
                greater = a.GetVarchar() > b.GetVarchar();
                break;
            case TypeId::DOUBLE:
                less = a.GetDouble() < b.GetDouble();
                greater = a.GetDouble() > b.GetDouble();
                break;
        }
        if (less || greater) { return order.ascending ? less : greater; }
    }
    return false;
}

}  // namespace


std::vector<Tuple> ExecutionOperator::Execute() {
    Init();
    std::vector<Tuple> rows;
    while (auto row = Next()) { rows.push_back(std::move(*row)); }
    return rows;
}

std::optional<Tuple> TableScanOperator::Next() {
    RequireInitialized();
    if (ended_) { return std::nullopt; }
    while (true) {
        const auto rid = started_ ? heap_.GetNextRID(*current_) : heap_.GetFirstRID();
        if (!rid) { ended_ = true; return std::nullopt; }
        current_ = rid;
        started_ = true;
        auto record = heap_.GetRecord(*rid);
        const auto meta = heap_.GetTupleMeta(*rid);
        if (context_ && context_->GetTransaction().GetIsolationLevel() !=
                            IsolationLevel::RepeatableRead) {
            auto* versions = context_->GetTransactionManager();
            if (versions == nullptr) {
                throw std::logic_error("Snapshot scan requires a TransactionManager");
            }
            auto visible = versions->ReconstructVersion(
                *rid, record, meta, context_->GetTransaction().GetReadTimestamp(),
                context_->GetTransaction().GetId());
            if (!visible) { continue; }
            record = std::move(visible->record);
        } else if (meta.is_deleted) {
            continue;
        }
        return Tuple::Deserialize(record, schema_);
    }
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

AggregateOperator::AggregateOperator(std::unique_ptr<ExecutionOperator> input, Schema source, Schema output,
    std::vector<PlanAggregate> specs, std::optional<std::size_t> group_by, bool project_group)
    : input_(std::move(input)), source_(std::move(source)), output_(std::move(output)),
      specs_(std::move(specs)), group_by_(group_by), project_group_(project_group) {
    if (!input_ || specs_.empty() || (project_group_ && !group_by_) ||
        specs_.size() + (project_group_ ? 1U : 0U) != output_.GetColumnCount() ||
        (group_by_ && *group_by_ >= source_.GetColumnCount())) {
        throw std::invalid_argument("Invalid aggregate operator");
    }
}

void AggregateOperator::Init() {
    initialized_ = false;
    rows_.clear();
    position_ = 0;
    input_->Init();
    const auto& source = source_;
    const auto& output = output_;
    const auto& specs = specs_;
    const auto group_by = group_by_;
    struct AggregateState {
        explicit AggregateState(std::size_t size)
            : values(size), counts(size, 0), sums(size, 0), average_sums(size, 0) {}
        std::optional<Value> group_key;
        std::vector<std::optional<Value>> values;
        std::vector<std::uint64_t> counts;
        std::vector<std::int64_t> sums;
        std::vector<long double> average_sums;
    };
    std::vector<AggregateState> states;
    if (!group_by) { states.emplace_back(specs.size()); }
    while (auto next = input_->Next()) {
        const auto& tuple = *next;
        std::size_t state_index = 0;
        if (group_by) {
            const auto& key = tuple.GetValue(*group_by);
            while (state_index < states.size() && states[state_index].group_key != key) {
                ++state_index;
            }
            if (state_index == states.size()) {
                states.emplace_back(specs.size());
                states.back().group_key = key;
            }
        }
        auto& state = states[state_index];
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            if (!spec.column_index) {
                if (spec.type != AggregateType::Count) {
                    throw std::invalid_argument("Invalid aggregate star argument");
                }
                ++state.counts[i];
                continue;
            }
            if (*spec.column_index >= source.GetColumnCount() ||
                source.GetColumn(*spec.column_index).GetType() != spec.input_type) {
                throw std::invalid_argument("Aggregate column does not match catalog");
            }
            const auto& value = tuple.GetValue(*spec.column_index);
            if (value.IsNull()) { continue; }
            ++state.counts[i];
            if (spec.type == AggregateType::Count) { continue; }
            if (spec.type == AggregateType::Sum) {
                const auto number = spec.input_type == TypeId::INTEGER
                    ? static_cast<std::int64_t>(value.GetInteger()) : value.GetBigInt();
                if ((number > 0 && state.sums[i] > std::numeric_limits<std::int64_t>::max() - number) ||
                    (number < 0 && state.sums[i] < std::numeric_limits<std::int64_t>::min() - number)) {
                    throw std::overflow_error("SUM overflow");
                }
                state.sums[i] += number;
            } else if (spec.type == AggregateType::Avg) {
                state.average_sums[i] += spec.input_type == TypeId::INTEGER
                    ? static_cast<long double>(value.GetInteger())
                    : static_cast<long double>(value.GetBigInt());
            } else if (!state.values[i] ||
                       (spec.type == AggregateType::Min && ValueLess(value, *state.values[i])) ||
                       (spec.type == AggregateType::Max && ValueLess(*state.values[i], value))) {
                state.values[i] = value;
            }
        }
    }
    for (std::size_t position = 0; position < states.size(); ++position) {
        const auto& state = states[position];
        std::vector<Value> row;
        row.reserve(output.GetColumnCount());
        if (project_group_) { row.push_back(*state.group_key); }
        for (std::size_t i = 0; i < specs.size(); ++i) {
            switch (specs[i].type) {
                case AggregateType::Count:
                    if (state.counts[i] > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                        throw std::overflow_error("COUNT overflow");
                    }
                    row.push_back(Value::BigInt(static_cast<std::int64_t>(state.counts[i])));
                    break;
                case AggregateType::Sum:
                    row.push_back(state.counts[i] == 0 ? Value::Null(TypeId::BIGINT) :
                        Value::BigInt(state.sums[i]));
                    break;
                case AggregateType::Avg:
                    row.push_back(state.counts[i] == 0 ? Value::Null(TypeId::DOUBLE) :
                        Value::Double(static_cast<double>(state.average_sums[i] / state.counts[i])));
                    break;
                case AggregateType::Min:
                case AggregateType::Max:
                    row.push_back(state.values[i].value_or(Value::Null(specs[i].input_type)));
                break;
            }
        }
        Tuple tuple(output, std::move(row));
        rows_.push_back(std::move(tuple));
    }
    initialized_ = true;
}

std::optional<Tuple> AggregateOperator::Next() {
    RequireInitialized();
    if (position_ == rows_.size()) { return std::nullopt; }
    return rows_[position_++];
}

SortOperator::SortOperator(std::unique_ptr<ExecutionOperator> input, std::vector<PlanOrderBy> order_by)
    : input_(std::move(input)), order_by_(std::move(order_by)) {
    if (!input_) { throw std::invalid_argument("Sort requires an input"); }
}

void SortOperator::Init() {
    initialized_ = false;
    rows_ = input_->Execute();
    position_ = 0;
    std::stable_sort(rows_.begin(), rows_.end(),
        [this](const Tuple& a, const Tuple& b) { return OrderLess(a, b, order_by_); });
    initialized_ = true;
}

std::optional<Tuple> SortOperator::Next() {
    RequireInitialized();
    if (position_ == rows_.size()) { return std::nullopt; }
    return rows_[position_++];
}

std::size_t JoinKeyHash::operator()(const Value& key) const {
    switch (key.GetType()) {
        case TypeId::BOOLEAN: return std::hash<bool>{}(key.GetBoolean());
        case TypeId::INTEGER: return std::hash<std::int32_t>{}(key.GetInteger());
        case TypeId::BIGINT: return std::hash<std::int64_t>{}(key.GetBigInt());
        case TypeId::VARCHAR: return std::hash<std::string>{}(key.GetVarchar());
        case TypeId::DOUBLE: break;
    }
    throw std::invalid_argument("Unsupported hash join key type");
}

JoinOperator::JoinOperator(std::unique_ptr<ExecutionOperator> left, std::unique_ptr<ExecutionOperator> right,
    Schema source, std::size_t left_columns, BoundExpressionPtr condition, JoinAlgorithm algorithm,
    std::optional<bool> smaller_input_is_left)
    : left_(std::move(left)), right_(std::move(right)), source_(std::move(source)),
      left_columns_(left_columns), condition_(std::move(condition)), algorithm_(algorithm),
      smaller_input_is_left_(smaller_input_is_left.value_or(algorithm != JoinAlgorithm::Hash)) {
    if (!left_ || !right_ || left_columns_ == 0 || left_columns_ >= source_.GetColumnCount()) {
        throw std::invalid_argument("Invalid join inputs");
    }
    if (algorithm_ == JoinAlgorithm::Hash) {
        const auto* equality = condition_ ? std::get_if<BoundComparisonExpression>(&condition_->node) : nullptr;
        if (!equality || equality->op != ComparisonOperator::Equal || !equality->left || !equality->right) {
            throw std::invalid_argument("Hash join requires column equality");
        }
        const auto* a = std::get_if<BoundColumnExpression>(&equality->left->node);
        const auto* b = std::get_if<BoundColumnExpression>(&equality->right->node);
        if (!a || !b || a->column_index >= source_.GetColumnCount() || b->column_index >= source_.GetColumnCount() ||
            (a->column_index < left_columns_) == (b->column_index < left_columns_) ||
            equality->left->type != equality->right->type || equality->left->type == TypeId::DOUBLE) {
            throw std::invalid_argument("Unsupported hash join columns");
        }
        left_key_ = a->column_index < left_columns_ ? a->column_index : b->column_index;
        right_key_ = (a->column_index < left_columns_ ? b->column_index : a->column_index) - left_columns_;
    }
}

void JoinOperator::Init() {
    initialized_ = false;
    left_->Init();
    right_->Init();
    buckets_.clear();
    outer_or_probe_row_.reset();
    matches_ = nullptr;
    match_position_ = 0;
    built_ = false;
    ended_ = false;
    initialized_ = true;
}

Tuple JoinOperator::Combine(const Tuple& left, const Tuple& right) const {
    std::vector<Value> values;
    values.reserve(source_.GetColumnCount());
    for (std::size_t i = 0; i < left_columns_; ++i) { values.push_back(left.GetValue(i)); }
    for (std::size_t i = left_columns_; i < source_.GetColumnCount(); ++i) {
        values.push_back(right.GetValue(i - left_columns_));
    }
    return Tuple(source_, std::move(values));
}

std::optional<Tuple> JoinOperator::Next() {
    RequireInitialized();
    if (ended_) { return std::nullopt; }
    if (algorithm_ == JoinAlgorithm::Hash && !built_) {
        auto& build = smaller_input_is_left_ ? left_ : right_;
        const auto key_index = smaller_input_is_left_ ? left_key_ : right_key_;
        while (auto row = build->Next()) {
            const auto key = row->GetValue(key_index);
            if (!key.IsNull()) { buckets_[key].push_back(std::move(*row)); }
        }
        built_ = true;
    }
    for (;;) {
        if (!outer_or_probe_row_) {
            auto& outer_or_probe = smaller_input_is_left_ ?
                (algorithm_ == JoinAlgorithm::Hash ? right_ : left_) :
                (algorithm_ == JoinAlgorithm::Hash ? left_ : right_);
            outer_or_probe_row_ = outer_or_probe->Next();
            if (!outer_or_probe_row_) { ended_ = true; return std::nullopt; }
            if (algorithm_ == JoinAlgorithm::Hash) {
                const auto key_index = smaller_input_is_left_ ? right_key_ : left_key_;
                const auto& key = outer_or_probe_row_->GetValue(key_index);
                if (key.IsNull()) { outer_or_probe_row_.reset(); continue; }
                const auto found = buckets_.find(key);
                if (found == buckets_.end()) { outer_or_probe_row_.reset(); continue; }
                matches_ = &found->second;
                match_position_ = 0;
            } else {
                (smaller_input_is_left_ ? right_ : left_)->Init();
            }
        }
        if (algorithm_ == JoinAlgorithm::Hash) {
            while (match_position_ < matches_->size()) {
                const auto& match = (*matches_)[match_position_++];
                auto tuple = smaller_input_is_left_ ? Combine(match, *outer_or_probe_row_) :
                                                       Combine(*outer_or_probe_row_, match);
                if (FilterOperator::Matches(condition_, tuple)) { return tuple; }
            }
        } else {
            auto& inner = smaller_input_is_left_ ? right_ : left_;
            while (auto row = inner->Next()) {
                auto tuple = smaller_input_is_left_ ? Combine(*outer_or_probe_row_, *row) :
                                                       Combine(*row, *outer_or_probe_row_);
                if (FilterOperator::Matches(condition_, tuple)) { return tuple; }
            }
        }
        outer_or_probe_row_.reset();
    }
}

}  // namespace udb::sql
