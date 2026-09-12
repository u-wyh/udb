#include "udb/sql/executor.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace udb::sql {
namespace {

struct JoinKeyHash {
    std::size_t operator()(const Value& key) const {
        switch (key.GetType()) {
            case TypeId::BOOLEAN: return std::hash<bool>{}(key.GetBoolean());
            case TypeId::INTEGER: return std::hash<std::int32_t>{}(key.GetInteger());
            case TypeId::BIGINT: return std::hash<std::int64_t>{}(key.GetBigInt());
            case TypeId::VARCHAR: return std::hash<std::string>{}(key.GetVarchar());
            case TypeId::DOUBLE: break;
        }
        throw std::invalid_argument("Unsupported hash join key type");
    }
};

bool SameColumn(const Column& a, const Column& b) {
    return a.GetName() == b.GetName() && a.GetType() == b.GetType() && a.GetMaxLength() == b.GetMaxLength();
}

Schema JoinSchema(const TableMetadata& left, const TableMetadata& right) {
    std::vector<Column> columns;
    for (const auto& column : left.GetSchema().GetColumns()) {
        columns.emplace_back(left.GetTableName() + "." + column.GetName(),
                             column.GetType(), column.GetMaxLength());
    }
    for (const auto& column : right.GetSchema().GetColumns()) {
        columns.emplace_back(right.GetTableName() + "." + column.GetName(),
                             column.GetType(), column.GetMaxLength());
    }
    return Schema(std::move(columns));
}

Tuple JoinTuples(const Tuple& left, const Schema& left_schema,
                 const Tuple& right, const Schema& right_schema,
                 const Schema& joined_schema) {
    std::vector<Value> values;
    values.reserve(joined_schema.GetColumnCount());
    for (std::size_t i = 0; i < left_schema.GetColumnCount(); ++i) {
        values.push_back(left.GetValue(i));
    }
    for (std::size_t i = 0; i < right_schema.GetColumnCount(); ++i) {
        values.push_back(right.GetValue(i));
    }
    return Tuple(joined_schema, std::move(values));
}

void CheckSchema(const Schema& planned, const Schema& actual) {
    if (planned.GetColumnCount() != actual.GetColumnCount()) {
        throw std::invalid_argument("Plan target schema does not match catalog");
    }
    for (std::size_t i = 0; i < planned.GetColumnCount(); ++i) {
        if (!SameColumn(planned.GetColumn(i), actual.GetColumn(i))) {
            throw std::invalid_argument("Plan target schema does not match catalog");
        }
    }
}

void CheckExpression(const BoundExpressionPtr& expression, const Schema& source) {
    if (!expression) { return; }
    if (const auto* column = std::get_if<BoundColumnExpression>(&expression->node)) {
        if (column->column_index >= source.GetColumnCount() ||
            source.GetColumn(column->column_index).GetType() != expression->type) {
            throw std::invalid_argument("Plan predicate column does not match catalog");
        }
        return;
    }
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression->node)) {
        if (literal->value.GetType() != expression->type) {
            throw std::invalid_argument("Plan predicate literal type mismatch");
        }
        return;
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression->node)) {
        if (!comparison->left || !comparison->right || expression->type != TypeId::BOOLEAN ||
            comparison->left->type != comparison->right->type) {
            throw std::invalid_argument("Invalid plan comparison predicate");
        }
        CheckExpression(comparison->left, source);
        CheckExpression(comparison->right, source);
        return;
    }
    if (const auto* arithmetic = std::get_if<BoundArithmeticExpression>(&expression->node)) {
        if (!arithmetic->left || !arithmetic->right ||
            (expression->type != TypeId::INTEGER && expression->type != TypeId::BIGINT) ||
            arithmetic->left->type != expression->type || arithmetic->right->type != expression->type) {
            throw std::invalid_argument("Invalid plan arithmetic expression");
        }
        CheckExpression(arithmetic->left, source);
        CheckExpression(arithmetic->right, source);
        return;
    }
    const auto& logical = std::get<BoundLogicalExpression>(expression->node);
    if (!logical.left || expression->type != TypeId::BOOLEAN || logical.left->type != TypeId::BOOLEAN ||
        (logical.op == LogicalOperator::Not ? static_cast<bool>(logical.right) :
         (!logical.right || logical.right->type != TypeId::BOOLEAN))) {
        throw std::invalid_argument("Invalid plan logical predicate");
    }
    CheckExpression(logical.left, source);
    CheckExpression(logical.right, source);
}

std::optional<std::int64_t> GetIndexKey(const Value& value) {
    if (value.IsNull()) { return std::nullopt; }
    if (value.GetType() == TypeId::INTEGER) {
        return static_cast<std::int64_t>(value.GetInteger());
    }
    if (value.GetType() == TypeId::BIGINT) { return value.GetBigInt(); }
    throw std::logic_error("Index column has an unsupported type");
}

struct IndexChange {
    index_id_t index_id;
    RID rid;
    std::optional<std::int64_t> old_key;
    std::optional<std::int64_t> new_key;
};

void CheckScan(const Schema& source, const Schema& output,
               const std::vector<std::size_t>& indexes,
               const BoundExpressionPtr& predicate,
               const std::vector<PlanOrderBy>& order_by,
               const std::vector<BoundExpressionPtr>& projections) {
    if (!projections.empty()) {
        if (!indexes.empty() || projections.size() != output.GetColumnCount()) {
            throw std::invalid_argument("Plan expression projection size mismatch");
        }
        for (std::size_t i = 0; i < projections.size(); ++i) {
            CheckExpression(projections[i], source);
            if (!projections[i] || projections[i]->type != output.GetColumn(i).GetType()) {
                throw std::invalid_argument("Plan expression projection type mismatch");
            }
        }
    } else if (indexes.size() != output.GetColumnCount()) {
        throw std::invalid_argument("Plan projection size mismatch");
    }
    for (std::size_t i = 0; projections.empty() && i < indexes.size(); ++i) {
        if (indexes[i] >= source.GetColumnCount() ||
            !SameColumn(source.GetColumn(indexes[i]), output.GetColumn(i))) {
            throw std::invalid_argument("Plan projection does not match catalog");
        }
    }
    if (predicate && predicate->type != TypeId::BOOLEAN) {
        throw std::invalid_argument("Plan predicate must be BOOLEAN");
    }
    CheckExpression(predicate, source);
    for (const auto& order : order_by) {
        if (order.column_index >= source.GetColumnCount()) {
            throw std::invalid_argument("Plan ORDER BY column does not match catalog");
        }
    }
}

bool Matches(const BoundExpressionPtr& predicate, const Tuple& tuple) {
    if (!predicate) { return true; }
    const auto value = EvaluateExpression(*predicate, tuple);
    if (value.GetType() != TypeId::BOOLEAN) {
        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
    }
    return !value.IsNull() && value.GetBoolean();
}

Tuple Project(const Tuple& tuple, const Schema& output,
              const std::vector<std::size_t>& indexes,
              const std::vector<BoundExpressionPtr>& projections) {
    std::vector<Value> values;
    values.reserve(output.GetColumnCount());
    if (!projections.empty()) {
        for (const auto& expression : projections) {
            values.push_back(EvaluateExpression(*expression, tuple));
        }
    } else {
        for (const auto index : indexes) { values.push_back(tuple.GetValue(index)); }
    }
    return Tuple(output, std::move(values));
}

bool ReachedLimit(const std::optional<std::size_t>& limit, std::size_t row_count) {
    return limit && row_count >= *limit;
}

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

void FinishScan(ExecutionResult& result, std::vector<Tuple> tuples,
                const Schema& output, const std::vector<std::size_t>& indexes,
                const std::vector<PlanOrderBy>& order_by,
                const std::optional<std::size_t>& limit, std::size_t offset,
                const std::vector<BoundExpressionPtr>& projections) {
    if (!order_by.empty()) {
        std::stable_sort(tuples.begin(), tuples.end(),
            [&order_by](const Tuple& a, const Tuple& b) { return OrderLess(a, b, order_by); });
    }
    for (std::size_t position = 0; position < tuples.size(); ++position) {
        if (position < offset) { continue; }
        if (ReachedLimit(limit, result.rows.size())) { break; }
        result.rows.push_back(Project(tuples[position], output, indexes, projections));
    }
}

}  // namespace

ExecutionResult Executor::Execute(const PlanNode& plan) {
    switch (plan.GetType()) {
        case PlanType::CreateTable: {
            const auto& create = dynamic_cast<const CreateTablePlan&>(plan);
            ExecutionResult result{PlanType::CreateTable};
            catalog_.CreateTable(create.GetTableName(), create.GetTableSchema());
            return result;
        }
        case PlanType::CreateIndex: {
            const auto& create = dynamic_cast<const CreateIndexPlan&>(plan);
            catalog_.CreateIndex(create.GetIndexName(), create.GetTableId(),
                                 create.GetColumnIndex());
            return ExecutionResult{PlanType::CreateIndex};
        }
        case PlanType::DropTable: {
            const auto& drop = dynamic_cast<const DropTablePlan&>(plan);
            catalog_.DropTable(drop.GetTableId());
            return ExecutionResult{PlanType::DropTable};
        }
        case PlanType::DropIndex: {
            const auto& drop = dynamic_cast<const DropIndexPlan&>(plan);
            catalog_.DropIndex(drop.GetIndexId());
            return ExecutionResult{PlanType::DropIndex};
        }
        case PlanType::Insert: {
            const auto& insert = dynamic_cast<const InsertPlan&>(plan);
            const auto& schema = catalog_.GetTable(insert.GetTableId()).GetSchema();
            CheckSchema(insert.GetTableSchema(), schema);
            const Tuple tuple(schema, insert.GetValues());
            const auto record = tuple.Serialize(schema);
            std::vector<std::pair<index_id_t, std::int64_t>> index_keys;
            for (const auto index_id : catalog_.GetTableIndexes(insert.GetTableId())) {
                const auto& index = catalog_.GetIndex(index_id);
                const auto key = GetIndexKey(tuple.GetValue(index.GetMetadata().GetColumnIndex()));
                if (!key) { continue; }
                if (index.GetTree().GetValue(*key)) {
                    throw std::invalid_argument("Unique index key already exists");
                }
                index_keys.emplace_back(index_id, *key);
            }
            ExecutionResult result{PlanType::Insert};
            result.inserted_rid = catalog_.GetTableHeap(insert.GetTableId()).InsertRecord(record);
            for (const auto& [index_id, key] : index_keys) {
                if (!catalog_.GetIndex(index_id).GetTree().Insert(key, *result.inserted_rid)) {
                    throw std::runtime_error("Index changed after INSERT uniqueness check");
                }
            }
            result.affected_rows = 1;
            return result;
        }
        case PlanType::SeqScan: {
            const auto& scan = dynamic_cast<const SeqScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            const auto& predicate = scan.GetPredicate();
            CheckScan(source, output, indexes, predicate, scan.GetOrderBy(), scan.GetProjections());
            ExecutionResult result{PlanType::SeqScan};
            result.output_schema = output;
            if (ReachedLimit(scan.GetLimit(), 0)) { return result; }
            std::vector<Tuple> tuples;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                if (Matches(predicate, tuple)) { tuples.push_back(tuple); }
            }
            FinishScan(result, std::move(tuples), output, indexes,
                       scan.GetOrderBy(), scan.GetLimit(), scan.GetOffset(), scan.GetProjections());
            return result;
        }
        case PlanType::IndexScan: {
            const auto& scan = dynamic_cast<const IndexScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            const auto& predicate = scan.GetPredicate();
            CheckScan(source, output, indexes, predicate, scan.GetOrderBy(), scan.GetProjections());
            const auto& index = catalog_.GetIndex(scan.GetIndexId());
            if (index.GetMetadata().GetTableId() != scan.GetTableId()) {
                throw std::invalid_argument("Index scan target does not match catalog");
            }
            ExecutionResult result{PlanType::IndexScan};
            result.output_schema = output;
            if (ReachedLimit(scan.GetLimit(), 0)) { return result; }
            const auto rid = index.GetTree().GetValue(scan.GetKey());
            if (!rid) { return result; }
            const auto tuple = Tuple::Deserialize(
                catalog_.GetTableHeap(scan.GetTableId()).GetRecord(*rid), source);
            std::vector<Tuple> tuples;
            if (Matches(predicate, tuple)) { tuples.push_back(tuple); }
            FinishScan(result, std::move(tuples), output, indexes,
                       scan.GetOrderBy(), scan.GetLimit(), scan.GetOffset(), scan.GetProjections());
            return result;
        }
        case PlanType::IndexRangeScan: {
            const auto& scan = dynamic_cast<const IndexRangeScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            const auto& predicate = scan.GetPredicate();
            CheckScan(source, output, indexes, predicate, scan.GetOrderBy(), scan.GetProjections());
            const auto& index = catalog_.GetIndex(scan.GetIndexId());
            if (index.GetMetadata().GetTableId() != scan.GetTableId()) {
                throw std::invalid_argument("Index range scan target does not match catalog");
            }
            ExecutionResult result{PlanType::IndexRangeScan};
            result.output_schema = output;
            if (ReachedLimit(scan.GetLimit(), 0)) { return result; }
            const auto entries = index.GetTree().ScanRange(
                scan.GetLowerBound(), scan.IsLowerInclusive(),
                scan.GetUpperBound(), scan.IsUpperInclusive());
            std::vector<Tuple> tuples;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (const auto& [key, rid] : entries) {
                static_cast<void>(key);
                const auto tuple = Tuple::Deserialize(heap.GetRecord(rid), source);
                if (Matches(predicate, tuple)) { tuples.push_back(tuple); }
            }
            FinishScan(result, std::move(tuples), output, indexes,
                       scan.GetOrderBy(), scan.GetLimit(), scan.GetOffset(), scan.GetProjections());
            return result;
        }
        case PlanType::CrossJoin:
        case PlanType::NestedLoopJoin:
        case PlanType::HashJoin: {
            const auto& join = dynamic_cast<const JoinPlan&>(plan);
            const auto& left = catalog_.GetTable(join.GetLeftTableId());
            const auto& right = catalog_.GetTable(join.GetRightTableId());
            const auto source = JoinSchema(left, right);
            const auto& output = join.GetOutputSchema();
            CheckScan(source, output, join.GetColumnIndexes(), join.GetPredicate(),
                      join.GetOrderBy(), join.GetProjections());
            CheckExpression(join.GetJoinCondition(), source);
            ExecutionResult result{plan.GetType()};
            result.output_schema = output;
            if (ReachedLimit(join.GetLimit(), 0)) { return result; }
            std::vector<Tuple> tuples;
            const auto& left_heap = catalog_.GetTableHeap(join.GetLeftTableId());
            const auto& right_heap = catalog_.GetTableHeap(join.GetRightTableId());
            std::unordered_map<Value, std::vector<Tuple>, JoinKeyHash> buckets;
            std::size_t left_key = 0;
            if (plan.GetType() == PlanType::HashJoin) {
                const auto* equality = std::get_if<BoundComparisonExpression>(&join.GetJoinCondition()->node);
                if (!equality || equality->op != ComparisonOperator::Equal) {
                    throw std::invalid_argument("Hash join requires column equality");
                }
                const auto* a = std::get_if<BoundColumnExpression>(&equality->left->node);
                const auto* b = std::get_if<BoundColumnExpression>(&equality->right->node);
                const auto boundary = left.GetSchema().GetColumnCount();
                if (!a || !b || (a->column_index < boundary) == (b->column_index < boundary) ||
                    equality->left->type == TypeId::DOUBLE) {
                    throw std::invalid_argument("Unsupported hash join columns");
                }
                left_key = a->column_index < boundary ? a->column_index : b->column_index;
                const auto right_key = (a->column_index < boundary ? b->column_index : a->column_index) - boundary;
                for (auto rid = right_heap.GetFirstRID(); rid; rid = right_heap.GetNextRID(*rid)) {
                    auto tuple = Tuple::Deserialize(right_heap.GetRecord(*rid), right.GetSchema());
                    const auto key = tuple.GetValue(right_key);
                    if (!key.IsNull()) { buckets[key].push_back(std::move(tuple)); }
                }
            }
            for (auto left_rid = left_heap.GetFirstRID(); left_rid;
                 left_rid = left_heap.GetNextRID(*left_rid)) {
                const auto left_tuple = Tuple::Deserialize(
                    left_heap.GetRecord(*left_rid), left.GetSchema());
                if (plan.GetType() == PlanType::HashJoin) {
                    const auto& key = left_tuple.GetValue(left_key);
                    if (key.IsNull()) { continue; }
                    const auto found = buckets.find(key);
                    if (found == buckets.end()) { continue; }
                    for (const auto& right_tuple : found->second) {
                        auto tuple = JoinTuples(left_tuple, left.GetSchema(), right_tuple,
                                                right.GetSchema(), source);
                        if (Matches(join.GetJoinCondition(), tuple) && Matches(join.GetPredicate(), tuple)) {
                            tuples.push_back(std::move(tuple));
                        }
                    }
                    continue;
                }
                for (auto right_rid = right_heap.GetFirstRID(); right_rid;
                     right_rid = right_heap.GetNextRID(*right_rid)) {
                    const auto right_tuple = Tuple::Deserialize(
                        right_heap.GetRecord(*right_rid), right.GetSchema());
                    auto tuple = JoinTuples(left_tuple, left.GetSchema(), right_tuple,
                                            right.GetSchema(), source);
                    if (Matches(join.GetJoinCondition(), tuple) && Matches(join.GetPredicate(), tuple)) {
                        tuples.push_back(std::move(tuple));
                    }
                }
            }
            FinishScan(result, std::move(tuples), output, join.GetColumnIndexes(),
                       join.GetOrderBy(), join.GetLimit(), join.GetOffset(), join.GetProjections());
            return result;
        }
        case PlanType::Aggregate: {
            const auto& aggregate = dynamic_cast<const AggregatePlan&>(plan);
            const auto& source = catalog_.GetTable(aggregate.GetTableId()).GetSchema();
            const auto& specs = aggregate.GetAggregates();
            const auto& output = aggregate.GetOutputSchema();
            const auto group_by = aggregate.GetGroupByColumn();
            if (specs.empty() || specs.size() + (aggregate.ProjectsGroupBy() ? 1U : 0U) != output.GetColumnCount() ||
                (group_by && *group_by >= source.GetColumnCount())) {
                throw std::invalid_argument("Invalid aggregate plan");
            }
            CheckExpression(aggregate.GetPredicate(), source);
            CheckExpression(aggregate.GetHaving(), output);
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
            const auto& heap = catalog_.GetTableHeap(aggregate.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                if (!Matches(aggregate.GetPredicate(), tuple)) { continue; }
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
            ExecutionResult result{PlanType::Aggregate};
            result.output_schema = output;
            std::size_t matched_groups = 0;
            for (std::size_t position = 0; position < states.size(); ++position) {
                const auto& state = states[position];
                std::vector<Value> row;
                row.reserve(output.GetColumnCount());
                if (aggregate.ProjectsGroupBy()) { row.push_back(*state.group_key); }
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
                if (!Matches(aggregate.GetHaving(), tuple)) { continue; }
                if (matched_groups++ < aggregate.GetOffset()) { continue; }
                if (ReachedLimit(aggregate.GetLimit(), result.rows.size())) { break; }
                result.rows.push_back(std::move(tuple));
            }
            return result;
        }
        case PlanType::Delete: {
            const auto& deletion = dynamic_cast<const DeletePlan&>(plan);
            const auto& source = catalog_.GetTable(deletion.GetTableId()).GetSchema();
            CheckSchema(deletion.GetTableSchema(), source);
            const auto& predicate = deletion.GetPredicate();
            if (predicate && predicate->type != TypeId::BOOLEAN) {
                throw std::invalid_argument("Plan predicate must be BOOLEAN");
            }
            CheckExpression(predicate, source);
            auto& heap = catalog_.GetTableHeap(deletion.GetTableId());
            struct DeleteMatch {
                RID rid;
                std::vector<std::pair<index_id_t, std::int64_t>> keys;
            };
            std::vector<DeleteMatch> matches;
            const auto table_indexes = catalog_.GetTableIndexes(deletion.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                bool remove = !predicate;
                std::optional<Tuple> tuple;
                if (predicate) {
                    tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                    const auto value = EvaluateExpression(*predicate, *tuple);
                    if (value.GetType() != TypeId::BOOLEAN) {
                        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
                    }
                    remove = !value.IsNull() && value.GetBoolean();
                }
                if (!remove) { continue; }
                if (!tuple) { tuple = Tuple::Deserialize(heap.GetRecord(*rid), source); }
                DeleteMatch match{*rid, {}};
                for (const auto index_id : table_indexes) {
                    const auto& index = catalog_.GetIndex(index_id);
                    const auto key = GetIndexKey(tuple->GetValue(index.GetMetadata().GetColumnIndex()));
                    if (key) { match.keys.emplace_back(index_id, *key); }
                }
                matches.push_back(std::move(match));
            }
            for (const auto& match : matches) {
                for (const auto& [index_id, key] : match.keys) {
                    if (!catalog_.GetIndex(index_id).GetTree().Remove(key)) {
                        throw std::runtime_error("Index is missing key for deleted tuple");
                    }
                }
                heap.DeleteRecord(match.rid);
            }
            ExecutionResult result{PlanType::Delete};
            result.affected_rows = matches.size();
            return result;
        }
        case PlanType::Update: {
            const auto& update = dynamic_cast<const UpdatePlan&>(plan);
            const auto& source = catalog_.GetTable(update.GetTableId()).GetSchema();
            CheckSchema(update.GetTableSchema(), source);
            if (update.GetAssignments().empty()) {
                throw std::invalid_argument("UPDATE plan has no assignments");
            }
            std::vector<bool> assigned(source.GetColumnCount(), false);
            for (const auto& assignment : update.GetAssignments()) {
                if (assignment.column_index >= source.GetColumnCount() || assigned[assignment.column_index]) {
                    throw std::invalid_argument("Invalid UPDATE assignment index");
                }
                assigned[assignment.column_index] = true;
                const auto& column = source.GetColumn(assignment.column_index);
                if (assignment.value.GetType() != column.GetType() ||
                    (!assignment.value.IsNull() && column.GetType() == TypeId::VARCHAR &&
                     assignment.value.GetVarchar().size() > column.GetMaxLength())) {
                    throw std::invalid_argument("UPDATE assignment does not match schema");
                }
            }
            const auto& predicate = update.GetPredicate();
            if (predicate && predicate->type != TypeId::BOOLEAN) {
                throw std::invalid_argument("Plan predicate must be BOOLEAN");
            }
            CheckExpression(predicate, source);
            auto& heap = catalog_.GetTableHeap(update.GetTableId());
            struct Replacement {
                RID rid;
                Record record;
                std::vector<IndexChange> index_changes;
            };
            std::vector<Replacement> replacements;
            const auto table_indexes = catalog_.GetTableIndexes(update.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                bool match = !predicate;
                if (predicate) {
                    const auto value = EvaluateExpression(*predicate, tuple);
                    if (value.GetType() != TypeId::BOOLEAN) {
                        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
                    }
                    match = !value.IsNull() && value.GetBoolean();
                }
                if (!match) { continue; }
                std::vector<Value> values;
                values.reserve(source.GetColumnCount());
                for (std::size_t i = 0; i < source.GetColumnCount(); ++i) { values.push_back(tuple.GetValue(i)); }
                for (const auto& assignment : update.GetAssignments()) {
                    values[assignment.column_index] = assignment.value;
                }
                const Tuple replacement(source, std::move(values));
                Replacement pending{*rid, replacement.Serialize(source), {}};
                for (const auto index_id : table_indexes) {
                    const auto& index = catalog_.GetIndex(index_id);
                    const auto column_index = index.GetMetadata().GetColumnIndex();
                    if (!assigned[column_index]) { continue; }
                    const auto old_key = GetIndexKey(tuple.GetValue(column_index));
                    const auto new_key = GetIndexKey(replacement.GetValue(column_index));
                    if (old_key != new_key) {
                        pending.index_changes.push_back(IndexChange{index_id, *rid, old_key, new_key});
                    }
                }
                replacements.push_back(std::move(pending));
            }

            std::map<index_id_t, std::map<std::int64_t, RID>> proposed_keys;
            for (const auto& replacement : replacements) {
                for (const auto& change : replacement.index_changes) {
                    if (!change.new_key) { continue; }
                    const auto [position, inserted] =
                        proposed_keys[change.index_id].emplace(*change.new_key, change.rid);
                    if (!inserted && position->second != change.rid) {
                        throw std::invalid_argument("UPDATE creates duplicate unique index keys");
                    }
                    const auto existing = catalog_.GetIndex(change.index_id).GetTree().GetValue(*change.new_key);
                    if (existing && *existing != change.rid) {
                        throw std::invalid_argument("Unique index key already exists");
                    }
                }
            }
            std::size_t affected = 0;
            for (const auto& replacement : replacements) {
                if (!heap.UpdateRecord(replacement.rid, replacement.record)) {
                    throw std::runtime_error("Updated record does not fit in its current page");
                }
                for (const auto& change : replacement.index_changes) {
                    auto& tree = catalog_.GetIndex(change.index_id).GetTree();
                    if (change.old_key && !tree.Remove(*change.old_key)) {
                        throw std::runtime_error("Index is missing old key for updated tuple");
                    }
                    if (change.new_key && !tree.Insert(*change.new_key, change.rid)) {
                        throw std::runtime_error("Index changed after UPDATE uniqueness check");
                    }
                }
                ++affected;
            }
            ExecutionResult result{PlanType::Update};
            result.affected_rows = affected;
            return result;
        }
    }
    throw std::invalid_argument("Unsupported plan type");
}

}  // namespace udb::sql
