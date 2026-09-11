#include "udb/sql/executor.h"

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

}  // namespace

ExecutionResult Executor::Execute(const PlanNode& plan) {
    switch (plan.GetType()) {
        case PlanType::CreateTable: {
            const auto& create = dynamic_cast<const CreateTablePlan&>(plan);
            ExecutionResult result{PlanType::CreateTable};
            catalog_.CreateTable(create.GetTableName(), create.GetTableSchema());
            return result;
        }
        case PlanType::Insert: {
            const auto& insert = dynamic_cast<const InsertPlan&>(plan);
            const auto& schema = catalog_.GetTable(insert.GetTableId()).GetSchema();
            CheckSchema(insert.GetTableSchema(), schema);
            const Tuple tuple(schema, insert.GetValues());
            const auto record = tuple.Serialize(schema);
            ExecutionResult result{PlanType::Insert};
            result.inserted_rid = catalog_.GetTableHeap(insert.GetTableId()).InsertRecord(record);
            result.affected_rows = 1;
            return result;
        }
        case PlanType::SeqScan: {
            const auto& scan = dynamic_cast<const SeqScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            if (indexes.size() != output.GetColumnCount()) {
                throw std::invalid_argument("Plan projection size mismatch");
            }
            for (std::size_t i = 0; i < indexes.size(); ++i) {
                if (indexes[i] >= source.GetColumnCount() || !SameColumn(source.GetColumn(indexes[i]), output.GetColumn(i))) {
                    throw std::invalid_argument("Plan projection does not match catalog");
                }
            }
            const auto& predicate = scan.GetPredicate();
            if (predicate && predicate->type != TypeId::BOOLEAN) {
                throw std::invalid_argument("Plan predicate must be BOOLEAN");
            }
            CheckExpression(predicate, source);
            ExecutionResult result{PlanType::SeqScan};
            result.output_schema = output;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                if (predicate) {
                    const auto value = EvaluateExpression(*predicate, tuple);
                    if (value.GetType() != TypeId::BOOLEAN) {
                        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
                    }
                    if (value.IsNull() || !value.GetBoolean()) { continue; }
                }
                std::vector<Value> values;
                values.reserve(indexes.size());
                for (const auto index : indexes) { values.push_back(tuple.GetValue(index)); }
                result.rows.emplace_back(output, std::move(values));
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
            std::vector<RID> matches;
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                bool remove = !predicate;
                if (predicate) {
                    const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                    const auto value = EvaluateExpression(*predicate, tuple);
                    if (value.GetType() != TypeId::BOOLEAN) {
                        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
                    }
                    remove = !value.IsNull() && value.GetBoolean();
                }
                if (remove) { matches.push_back(*rid); }
            }
            for (const auto rid : matches) { heap.DeleteRecord(rid); }
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
            std::vector<std::pair<RID, Record>> replacements;
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
                replacements.emplace_back(*rid, Tuple(source, std::move(values)).Serialize(source));
            }
            std::size_t affected = 0;
            for (const auto& replacement : replacements) {
                if (!heap.UpdateRecord(replacement.first, replacement.second)) {
                    throw std::runtime_error("Updated record does not fit in its current page");
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
