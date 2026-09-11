#include "udb/sql/executor.h"

#include <map>

namespace udb::sql {
namespace {

bool SameColumn(const Column& a, const Column& b) {
    return a.GetName() == b.GetName() && a.GetType() == b.GetType() && a.GetMaxLength() == b.GetMaxLength();
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
               const BoundExpressionPtr& predicate) {
    if (indexes.size() != output.GetColumnCount()) {
        throw std::invalid_argument("Plan projection size mismatch");
    }
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i] >= source.GetColumnCount() ||
            !SameColumn(source.GetColumn(indexes[i]), output.GetColumn(i))) {
            throw std::invalid_argument("Plan projection does not match catalog");
        }
    }
    if (predicate && predicate->type != TypeId::BOOLEAN) {
        throw std::invalid_argument("Plan predicate must be BOOLEAN");
    }
    CheckExpression(predicate, source);
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
              const std::vector<std::size_t>& indexes) {
    std::vector<Value> values;
    values.reserve(indexes.size());
    for (const auto index : indexes) { values.push_back(tuple.GetValue(index)); }
    return Tuple(output, std::move(values));
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
            CheckScan(source, output, indexes, predicate);
            ExecutionResult result{PlanType::SeqScan};
            result.output_schema = output;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                if (Matches(predicate, tuple)) { result.rows.push_back(Project(tuple, output, indexes)); }
            }
            return result;
        }
        case PlanType::IndexScan: {
            const auto& scan = dynamic_cast<const IndexScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            const auto& predicate = scan.GetPredicate();
            CheckScan(source, output, indexes, predicate);
            const auto& index = catalog_.GetIndex(scan.GetIndexId());
            if (index.GetMetadata().GetTableId() != scan.GetTableId()) {
                throw std::invalid_argument("Index scan target does not match catalog");
            }
            ExecutionResult result{PlanType::IndexScan};
            result.output_schema = output;
            const auto rid = index.GetTree().GetValue(scan.GetKey());
            if (!rid) { return result; }
            const auto tuple = Tuple::Deserialize(
                catalog_.GetTableHeap(scan.GetTableId()).GetRecord(*rid), source);
            if (Matches(predicate, tuple)) { result.rows.push_back(Project(tuple, output, indexes)); }
            return result;
        }
        case PlanType::IndexRangeScan: {
            const auto& scan = dynamic_cast<const IndexRangeScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            const auto& predicate = scan.GetPredicate();
            CheckScan(source, output, indexes, predicate);
            const auto& index = catalog_.GetIndex(scan.GetIndexId());
            if (index.GetMetadata().GetTableId() != scan.GetTableId()) {
                throw std::invalid_argument("Index range scan target does not match catalog");
            }
            ExecutionResult result{PlanType::IndexRangeScan};
            result.output_schema = output;
            const auto entries = index.GetTree().ScanRange(
                scan.GetLowerBound(), scan.IsLowerInclusive(),
                scan.GetUpperBound(), scan.IsUpperInclusive());
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (const auto& [key, rid] : entries) {
                static_cast<void>(key);
                const auto tuple = Tuple::Deserialize(heap.GetRecord(rid), source);
                if (Matches(predicate, tuple)) {
                    result.rows.push_back(Project(tuple, output, indexes));
                }
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
