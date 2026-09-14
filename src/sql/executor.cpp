#include "udb/sql/executor.h"
#include "udb/sql/operator.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace udb::sql {
namespace {

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

struct IndexChange {
    index_id_t index_id;
    RID rid;
    std::optional<IndexKey> old_key;
    std::optional<IndexKey> new_key;
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
    return FilterOperator::Matches(predicate, tuple);
}

TransactionManager* SnapshotVersions(const ExecutionContext* context) {
    if (context == nullptr) { return nullptr; }
    auto* versions = context->GetTransactionManager();
    return versions;
}

std::vector<RID> VersionAwareExactLookup(const Index& index, const IndexKey& key,
                                         const ExecutionContext* context) {
    auto rids = index.GetTree().GetValues(key);
    if (auto* versions = SnapshotVersions(context)) {
        for (const auto& [stale_key, rid] :
             versions->GetStaleIndexEntries(index.GetMetadata().GetIndexId())) {
            if (stale_key == key) { rids.push_back(rid); }
        }
    }
    std::sort(rids.begin(), rids.end());
    rids.erase(std::unique(rids.begin(), rids.end()), rids.end());
    return rids;
}

bool InIndexRange(const IndexKey& key, const std::optional<IndexKey>& lower,
                  bool lower_inclusive, const std::optional<IndexKey>& upper,
                  bool upper_inclusive) {
    if (lower && (key < *lower || (key == *lower && !lower_inclusive))) { return false; }
    if (upper && (key > *upper || (key == *upper && !upper_inclusive))) { return false; }
    return true;
}

std::vector<std::pair<IndexKey, RID>> VersionAwareRangeLookup(
    const Index& index, const std::optional<IndexKey>& lower, bool lower_inclusive,
    const std::optional<IndexKey>& upper, bool upper_inclusive,
    const ExecutionContext* context) {
    auto entries = index.GetTree().ScanKeys(lower, lower_inclusive, upper, upper_inclusive);
    if (auto* versions = SnapshotVersions(context)) {
        for (const auto& entry : versions->GetStaleIndexEntries(
                 index.GetMetadata().GetIndexId())) {
            if (InIndexRange(entry.first, lower, lower_inclusive, upper, upper_inclusive)) {
                entries.push_back(entry);
            }
        }
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    return entries;
}

std::optional<Tuple> ReadTuple(const TableHeap& heap, RID rid, const Schema& schema,
                               const ExecutionContext* context) {
    auto record = heap.GetRecord(rid);
    const auto meta = heap.GetTupleMeta(rid);
    if (context && context->GetTransactionManager()) {
        auto* versions = context->GetTransactionManager();
        if (versions == nullptr) {
            throw std::logic_error("Snapshot read requires a TransactionManager");
        }
        auto visible = versions->ReconstructVersion(
            rid, record, meta, context->GetTransaction().GetReadTimestamp(),
            context->GetTransaction().GetId());
        if (!visible) { return std::nullopt; }
        record = std::move(visible->record);
        if (context->GetTransaction().GetIsolationLevel() == IsolationLevel::Serializable) {
            versions->RegisterTupleRead(context->GetTransaction(), rid);
        }
    } else if (meta.is_deleted) {
        return std::nullopt;
    }
    return Tuple::Deserialize(record, schema);
}

bool ReachedLimit(const std::optional<std::size_t>& limit, std::size_t row_count) {
    return limit && row_count >= *limit;
}

Value IndexKeyValue(const IndexKey& key, TypeId type) {
    switch (type) {
        case TypeId::INTEGER: return Value::Integer(static_cast<std::int32_t>(key.GetInteger()));
        case TypeId::BIGINT: return Value::BigInt(key.GetInteger());
        case TypeId::VARCHAR: return Value::Varchar(key.GetString());
        default: throw std::invalid_argument("Index-only scan has an unsupported key type");
    }
}

Tuple IndexSourceTuple(const Schema& schema, std::size_t column_index, const Value& value) {
    std::vector<Value> values;
    values.reserve(schema.GetColumnCount());
    for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) {
        values.push_back(i == column_index ? value : Value::Null(schema.GetColumn(i).GetType()));
    }
    return Tuple(schema, std::move(values));
}

void FinishPipeline(ExecutionResult& result, std::unique_ptr<ExecutionOperator> input,
                const Schema& output, const std::vector<std::size_t>& indexes,
                const std::vector<PlanOrderBy>& order_by,
                const std::optional<std::size_t>& limit, std::size_t offset,
                const std::vector<BoundExpressionPtr>& projections) {
    if (!order_by.empty()) { input = std::make_unique<SortOperator>(std::move(input), order_by); }
    auto limited = std::make_unique<LimitOperator>(std::move(input), limit, offset);
    ProjectionOperator projection(std::move(limited), output, indexes, projections);
    result.rows = projection.Execute();
}

class StatementLockGuard {
public:
    explicit StatementLockGuard(ExecutionContext& context)
        : locks_(context.GetLockManager()), transaction_(context.GetTransaction()) {}
    StatementLockGuard(const StatementLockGuard&) = delete;
    StatementLockGuard& operator=(const StatementLockGuard&) = delete;
    ~StatementLockGuard() {
        if (locks_ == nullptr) { return; }
        for (auto table = release_tables_.rbegin(); table != release_tables_.rend(); ++table) {
            try { locks_->UnlockTable(transaction_, *table); } catch (...) {}
        }
    }
    void LockTable(table_id_t id, LockMode mode) {
        if (locks_ == nullptr) { return; }
        if (mode == LockMode::Shared) { return; }
        locks_->LockTable(transaction_, mode, id);
    }

private:
    LockManager* locks_;
    Transaction& transaction_;
    std::vector<table_id_t> release_tables_;
};

void AcquireExecutionLocks(const PlanNode& plan, StatementLockGuard& locks,
                           Catalog& catalog) {
    const auto lock_table = [&](table_id_t id, LockMode mode) { locks.LockTable(id, mode); };
    switch (plan.GetType()) {
        case PlanType::Insert:
            lock_table(dynamic_cast<const InsertPlan&>(plan).GetTableId(), LockMode::Exclusive);
            return;
        case PlanType::Delete:
            lock_table(dynamic_cast<const DeletePlan&>(plan).GetTableId(), LockMode::Exclusive);
            return;
        case PlanType::Update:
            lock_table(dynamic_cast<const UpdatePlan&>(plan).GetTableId(), LockMode::Exclusive);
            return;
        case PlanType::SeqScan:
            lock_table(dynamic_cast<const SeqScanPlan&>(plan).GetTableId(), LockMode::Shared);
            return;
        case PlanType::IndexScan:
            lock_table(dynamic_cast<const IndexScanPlan&>(plan).GetTableId(), LockMode::Shared);
            return;
        case PlanType::IndexRangeScan:
            lock_table(dynamic_cast<const IndexRangeScanPlan&>(plan).GetTableId(), LockMode::Shared);
            return;
        case PlanType::IndexOnlyScan:
            lock_table(dynamic_cast<const IndexOnlyScanPlan&>(plan).GetTableId(), LockMode::Shared);
            return;
        case PlanType::Aggregate:
            lock_table(dynamic_cast<const AggregatePlan&>(plan).GetTableId(), LockMode::Shared);
            return;
        case PlanType::CrossJoin:
        case PlanType::NestedLoopJoin:
        case PlanType::HashJoin: {
            const auto& join = dynamic_cast<const JoinPlan&>(plan);
            const auto first = std::min(join.GetLeftTableId(), join.GetRightTableId());
            const auto second = std::max(join.GetLeftTableId(), join.GetRightTableId());
            lock_table(first, LockMode::Shared);
            if (second != first) { lock_table(second, LockMode::Shared); }
            return;
        }
        case PlanType::CreateIndex:
            lock_table(dynamic_cast<const CreateIndexPlan&>(plan).GetTableId(), LockMode::Exclusive);
            return;
        case PlanType::DropTable:
            lock_table(dynamic_cast<const DropTablePlan&>(plan).GetTableId(), LockMode::Exclusive);
            return;
        case PlanType::DropIndex: {
            const auto index_id = dynamic_cast<const DropIndexPlan&>(plan).GetIndexId();
            lock_table(catalog.GetIndex(index_id).GetMetadata().GetTableId(), LockMode::Exclusive);
            return;
        }
        case PlanType::CreateTable:
        case PlanType::Begin:
        case PlanType::Commit:
        case PlanType::Rollback:
            return;
    }
}

void RegisterSerializablePlanReads(const PlanNode& plan, ExecutionContext& context) {
    auto* manager = context.GetTransactionManager();
    auto& transaction = context.GetTransaction();
    if (manager == nullptr || transaction.GetIsolationLevel() != IsolationLevel::Serializable) {
        return;
    }
    switch (plan.GetType()) {
        case PlanType::SeqScan:
            manager->RegisterTableRead(transaction,
                dynamic_cast<const SeqScanPlan&>(plan).GetTableId());
            return;
        case PlanType::IndexScan: {
            const auto& scan = dynamic_cast<const IndexScanPlan&>(plan);
            manager->RegisterIndexRead(transaction, scan.GetTableId(), scan.GetIndexId(),
                                       scan.GetKey(), true, scan.GetKey(), true);
            return;
        }
        case PlanType::IndexRangeScan: {
            const auto& scan = dynamic_cast<const IndexRangeScanPlan&>(plan);
            manager->RegisterIndexRead(transaction, scan.GetTableId(), scan.GetIndexId(),
                                       scan.GetLowerBound(), scan.IsLowerInclusive(),
                                       scan.GetUpperBound(), scan.IsUpperInclusive());
            return;
        }
        case PlanType::IndexOnlyScan: {
            const auto& scan = dynamic_cast<const IndexOnlyScanPlan&>(plan);
            const auto lower = scan.GetExactKey() ? scan.GetExactKey() : scan.GetLowerBound();
            const auto upper = scan.GetExactKey() ? scan.GetExactKey() : scan.GetUpperBound();
            manager->RegisterIndexRead(transaction, scan.GetTableId(), scan.GetIndexId(),
                                       lower, scan.GetExactKey() ? true : scan.IsLowerInclusive(),
                                       upper, scan.GetExactKey() ? true : scan.IsUpperInclusive());
            return;
        }
        case PlanType::Aggregate:
            manager->RegisterTableRead(transaction,
                dynamic_cast<const AggregatePlan&>(plan).GetTableId());
            return;
        case PlanType::CrossJoin:
        case PlanType::NestedLoopJoin:
        case PlanType::HashJoin: {
            const auto& join = dynamic_cast<const JoinPlan&>(plan);
            manager->RegisterTableRead(transaction, join.GetLeftTableId());
            manager->RegisterTableRead(transaction, join.GetRightTableId());
            return;
        }
        default:
            return;
    }
}

bool WritesPages(PlanType type) {
    return type == PlanType::CreateTable || type == PlanType::CreateIndex ||
           type == PlanType::DropTable || type == PlanType::DropIndex ||
           type == PlanType::Insert || type == PlanType::Delete || type == PlanType::Update;
}

}  // namespace

ExecutionResult Executor::Execute(const PlanNode& plan) {
    return ExecutePlan(plan, nullptr);
}

ExecutionResult Executor::ExecutePlan(const PlanNode& plan, ExecutionContext* context) {
    switch (plan.GetType()) {
        case PlanType::Begin:
        case PlanType::Commit:
        case PlanType::Rollback:
            throw std::invalid_argument("Transaction commands are executed by SqlEngine");
        case PlanType::CreateTable: {
            const auto& create = dynamic_cast<const CreateTablePlan&>(plan);
            ExecutionResult result{PlanType::CreateTable};
            catalog_.CreateTable(create.GetTableName(), create.GetTableSchema());
            return result;
        }
        case PlanType::CreateIndex: {
            const auto& create = dynamic_cast<const CreateIndexPlan&>(plan);
            catalog_.CreateIndex(create.GetIndexName(), create.GetTableId(),
                                 create.GetColumnIndexes());
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
            std::vector<std::pair<index_id_t, IndexKey>> index_keys;
            for (const auto index_id : catalog_.GetTableIndexes(insert.GetTableId())) {
                const auto& index = catalog_.GetIndex(index_id);
                const auto key = GetTupleIndexKey(tuple, index.GetMetadata().GetColumnIndexes());
                if (!key) { continue; }
                index.GetTree().ValidateKey(*key);
                if (index.GetTree().IsUnique() && index.GetTree().GetValue(*key)) {
                    throw std::invalid_argument("Unique index key already exists");
                }
                index_keys.emplace_back(index_id, *key);
            }
            ExecutionResult result{PlanType::Insert};
            auto* versions = context ? context->GetTransactionManager() : nullptr;
            const auto meta = versions
                ? TupleMeta{TransactionManager::EncodeTransactionTimestamp(
                                context->GetTransaction().GetId()), false}
                : TupleMeta{};
            if (versions && context->GetTransaction().GetIsolationLevel() ==
                                IsolationLevel::Serializable) {
                versions->RegisterTableWrite(context->GetTransaction(), insert.GetTableId());
                for (const auto& [index_id, key] : index_keys) {
                    versions->RegisterIndexWrite(context->GetTransaction(), insert.GetTableId(),
                                                 index_id, key);
                }
            }
            result.inserted_rid =
                catalog_.GetTableHeap(insert.GetTableId()).InsertRecord(record, meta);
            for (const auto& [index_id, key] : index_keys) {
                if (!catalog_.GetIndex(index_id).GetTree().Insert(key, *result.inserted_rid)) {
                    throw std::runtime_error("Index changed after INSERT uniqueness check");
                }
            }
            if (versions) { versions->RegisterWrite(context->GetTransaction(), *result.inserted_rid); }
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
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            auto filter = std::make_unique<FilterOperator>(
                std::make_unique<TableScanOperator>(heap, source, context), predicate);
            FinishPipeline(result, std::move(filter), output, indexes,
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
            std::vector<Tuple> tuples;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (const auto rid : VersionAwareExactLookup(index, scan.GetKey(), context)) {
                const auto tuple = ReadTuple(heap, rid, source, context);
                if (tuple && GetTupleIndexKey(*tuple, index.GetMetadata().GetColumnIndexes()) ==
                                 std::optional<IndexKey>(scan.GetKey()) && Matches(predicate, *tuple)) {
                    tuples.push_back(*tuple);
                }
            }
            FinishPipeline(result, std::make_unique<MaterializedOperator>(std::move(tuples)), output, indexes,
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
            const auto entries = VersionAwareRangeLookup(
                index, scan.GetLowerBound(), scan.IsLowerInclusive(),
                scan.GetUpperBound(), scan.IsUpperInclusive(), context);
            std::vector<Tuple> tuples;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (const auto& [key, rid] : entries) {
                const auto tuple = ReadTuple(heap, rid, source, context);
                if (tuple && GetTupleIndexKey(*tuple, index.GetMetadata().GetColumnIndexes()) ==
                                 std::optional<IndexKey>(key) && Matches(predicate, *tuple)) {
                    tuples.push_back(*tuple);
                }
            }
            FinishPipeline(result, std::make_unique<MaterializedOperator>(std::move(tuples)), output, indexes,
                       scan.GetOrderBy(), scan.GetLimit(), scan.GetOffset(), scan.GetProjections());
            return result;
        }
        case PlanType::IndexOnlyScan: {
            const auto& scan = dynamic_cast<const IndexOnlyScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& index = catalog_.GetIndex(scan.GetIndexId());
            const auto& columns = index.GetMetadata().GetColumnIndexes();
            if (index.GetMetadata().GetTableId() != scan.GetTableId() || columns.size() != 1 ||
                columns[0] != scan.GetColumnIndex() || output.GetColumnCount() != 1 ||
                output.GetColumn(0).GetType() != source.GetColumn(columns[0]).GetType()) {
                throw std::invalid_argument("Index-only scan does not match catalog");
            }
            CheckExpression(scan.GetPredicate(), source);
            ExecutionResult result{PlanType::IndexOnlyScan};
            result.output_schema = output;
            if (ReachedLimit(scan.GetLimit(), 0)) { return result; }
            if (context && context->GetTransactionManager()) {
                struct CoveringCandidate {
                    IndexKey key;
                    RID rid;
                    bool current;
                };
                std::vector<CoveringCandidate> entries;
                if (scan.GetExactKey()) {
                    for (const auto rid : index.GetTree().GetValues(*scan.GetExactKey())) {
                        entries.push_back({*scan.GetExactKey(), rid, true});
                    }
                } else {
                    for (const auto& [key, rid] : index.GetTree().ScanKeys(
                             scan.GetLowerBound(), scan.IsLowerInclusive(),
                             scan.GetUpperBound(), scan.IsUpperInclusive())) {
                        entries.push_back({key, rid, true});
                    }
                }
                auto* versions = SnapshotVersions(context);
                for (const auto& [key, rid] : versions->GetStaleIndexEntries(
                         index.GetMetadata().GetIndexId())) {
                    const bool selected = scan.GetExactKey()
                        ? key == *scan.GetExactKey()
                        : InIndexRange(key, scan.GetLowerBound(), scan.IsLowerInclusive(),
                                       scan.GetUpperBound(), scan.IsUpperInclusive());
                    if (selected) { entries.push_back({key, rid, false}); }
                }
                std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
                    if (left.key != right.key) { return left.key < right.key; }
                    if (left.rid != right.rid) { return left.rid < right.rid; }
                    return left.current > right.current;
                });
                entries.erase(std::unique(entries.begin(), entries.end(),
                    [](const auto& left, const auto& right) {
                        return left.key == right.key && left.rid == right.rid;
                    }), entries.end());
                std::vector<Tuple> rows;
                const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
                const auto& transaction = context->GetTransaction();
                for (const auto& entry : entries) {
                    const auto meta = heap.GetTupleMeta(entry.rid);
                    const bool owned = TransactionManager::IsTransactionTimestamp(meta.timestamp) &&
                        TransactionManager::DecodeTransactionTimestamp(meta.timestamp) ==
                            transaction.GetId();
                    const bool committed_visible =
                        !TransactionManager::IsTransactionTimestamp(meta.timestamp) &&
                        meta.timestamp <= transaction.GetReadTimestamp();
                    if (entry.current && !meta.is_deleted && (owned || committed_visible)) {
                        const auto value = IndexKeyValue(
                            entry.key, source.GetColumn(columns[0]).GetType());
                        if (Matches(scan.GetPredicate(),
                                    IndexSourceTuple(source, columns[0], value))) {
                            rows.emplace_back(output, std::vector<Value>{value});
                        }
                        continue;
                    }
                    const auto tuple = ReadTuple(heap, entry.rid, source, context);
                    if (tuple && GetTupleIndexKey(*tuple, columns) ==
                                     std::optional<IndexKey>(entry.key) &&
                        Matches(scan.GetPredicate(), *tuple)) {
                        rows.emplace_back(output,
                                          std::vector<Value>{tuple->GetValue(columns[0])});
                    }
                }
                auto limited = std::make_unique<LimitOperator>(
                    std::make_unique<MaterializedOperator>(std::move(rows)),
                    scan.GetLimit(), scan.GetOffset());
                result.rows = limited->Execute();
                return result;
            }
            std::vector<IndexKey> keys;
            if (scan.GetExactKey()) {
                keys.assign(index.GetTree().GetValues(*scan.GetExactKey()).size(), *scan.GetExactKey());
            } else {
                for (const auto& [key, rid] : index.GetTree().ScanKeys(
                         scan.GetLowerBound(), scan.IsLowerInclusive(),
                         scan.GetUpperBound(), scan.IsUpperInclusive())) {
                    static_cast<void>(rid);
                    keys.push_back(key);
                }
            }
            std::vector<Tuple> rows;
            for (const auto& key : keys) {
                const auto value = IndexKeyValue(key, source.GetColumn(columns[0]).GetType());
                if (Matches(scan.GetPredicate(), IndexSourceTuple(source, columns[0], value))) {
                    rows.emplace_back(output, std::vector<Value>{value});
                }
            }
            auto limited = std::make_unique<LimitOperator>(
                std::make_unique<MaterializedOperator>(std::move(rows)),
                scan.GetLimit(), scan.GetOffset());
            result.rows = limited->Execute();
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
            auto joined = std::make_unique<JoinOperator>(
                std::make_unique<TableScanOperator>(catalog_.GetTableHeap(join.GetLeftTableId()), left.GetSchema(), context),
                std::make_unique<TableScanOperator>(catalog_.GetTableHeap(join.GetRightTableId()), right.GetSchema(), context),
                source, left.GetSchema().GetColumnCount(), join.GetJoinCondition(),
                plan.GetType() == PlanType::HashJoin ? JoinAlgorithm::Hash : JoinAlgorithm::NestedLoop,
                join.IsSmallerInputLeft());
            auto filtered = std::make_unique<FilterOperator>(std::move(joined), join.GetPredicate());
            FinishPipeline(result, std::move(filtered), output, join.GetColumnIndexes(),
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
            const auto& heap = catalog_.GetTableHeap(aggregate.GetTableId());
            auto filtered = std::make_unique<FilterOperator>(
                std::make_unique<TableScanOperator>(heap, source, context), aggregate.GetPredicate());
            auto grouped = std::make_unique<AggregateOperator>(std::move(filtered), source, output,
                specs, group_by, aggregate.ProjectsGroupBy());
            auto having = std::make_unique<FilterOperator>(std::move(grouped), aggregate.GetHaving());
            LimitOperator limited(std::move(having), aggregate.GetLimit(), aggregate.GetOffset());
            ExecutionResult result{PlanType::Aggregate};
            result.output_schema = output;
            result.rows = limited.Execute();
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
                std::vector<std::pair<index_id_t, IndexKey>> keys;
            };
            std::vector<DeleteMatch> matches;
            const auto table_indexes = catalog_.GetTableIndexes(deletion.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = ReadTuple(heap, *rid, source, context);
                if (!tuple) { continue; }
                bool remove = !predicate;
                if (predicate) {
                    const auto value = EvaluateExpression(*predicate, *tuple);
                    if (value.GetType() != TypeId::BOOLEAN) {
                        throw std::invalid_argument("Predicate did not evaluate to BOOLEAN");
                    }
                    remove = !value.IsNull() && value.GetBoolean();
                }
                if (!remove) { continue; }
                DeleteMatch match{*rid, {}};
                for (const auto index_id : table_indexes) {
                    const auto& index = catalog_.GetIndex(index_id);
                    const auto key = GetTupleIndexKey(*tuple, index.GetMetadata().GetColumnIndexes());
                    if (key) { match.keys.emplace_back(index_id, *key); }
                }
                matches.push_back(std::move(match));
            }
            auto* versions = context ? context->GetTransactionManager() : nullptr;
            if (versions && !matches.empty() && context->GetTransaction().GetIsolationLevel() ==
                                                    IsolationLevel::Serializable) {
                versions->RegisterTableWrite(context->GetTransaction(), deletion.GetTableId());
            }
            for (const auto& match : matches) {
                if (versions) {
                    versions->RegisterTupleWrite(context->GetTransaction(), match.rid);
                    versions->CheckWriteConflict(context->GetTransaction(),
                                                 heap.GetTupleMeta(match.rid));
                }
                for (const auto& [index_id, key] : match.keys) {
                    if (versions) {
                        versions->RegisterIndexWrite(context->GetTransaction(),
                                                     deletion.GetTableId(), index_id, key);
                        versions->RegisterStaleIndexEntry(context->GetTransaction(),
                                                          index_id, key, match.rid);
                    }
                    if (!catalog_.GetIndex(index_id).GetTree().Remove(key, match.rid)) {
                        throw std::runtime_error("Index is missing key for deleted tuple");
                    }
                }
                if (versions) {
                    versions->AppendUndoRecord(context->GetTransaction(), match.rid,
                                               heap.GetRecord(match.rid),
                                               heap.GetTupleMeta(match.rid));
                    heap.SetTupleMeta(
                        match.rid,
                        {TransactionManager::EncodeTransactionTimestamp(
                             context->GetTransaction().GetId()), true});
                    versions->RegisterWrite(context->GetTransaction(), match.rid);
                } else {
                    heap.DeleteRecord(match.rid);
                }
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
                Record old_record;
                TupleMeta old_meta;
                std::vector<IndexChange> index_changes;
            };
            std::vector<Replacement> replacements;
            const auto table_indexes = catalog_.GetTableIndexes(update.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto visible = ReadTuple(heap, *rid, source, context);
                if (!visible) { continue; }
                const auto& tuple = *visible;
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
                Replacement pending{*rid, replacement.Serialize(source),
                                    heap.GetRecord(*rid), heap.GetTupleMeta(*rid), {}};
                for (const auto index_id : table_indexes) {
                    const auto& index = catalog_.GetIndex(index_id);
                    const auto& columns = index.GetMetadata().GetColumnIndexes();
                    if (std::none_of(columns.begin(), columns.end(), [&](std::size_t i) { return assigned[i]; })) { continue; }
                    const auto old_key = GetTupleIndexKey(tuple, columns);
                    const auto new_key = GetTupleIndexKey(replacement, columns);
                    if (new_key) { index.GetTree().ValidateKey(*new_key); }
                    if (old_key != new_key) {
                        pending.index_changes.push_back(IndexChange{index_id, *rid, old_key, new_key});
                    }
                }
                replacements.push_back(std::move(pending));
            }

            auto* versions = context ? context->GetTransactionManager() : nullptr;
            std::map<index_id_t, std::map<IndexKey, RID>> proposed_keys;
            if (versions) {
                if (!replacements.empty() && context->GetTransaction().GetIsolationLevel() ==
                                                 IsolationLevel::Serializable) {
                    versions->RegisterTableWrite(context->GetTransaction(), update.GetTableId());
                }
                for (const auto& replacement : replacements) {
                    versions->RegisterTupleWrite(context->GetTransaction(), replacement.rid);
                    versions->CheckWriteConflict(context->GetTransaction(),
                                                 replacement.old_meta);
                    for (const auto& change : replacement.index_changes) {
                        if (change.old_key) {
                            versions->RegisterIndexWrite(context->GetTransaction(),
                                update.GetTableId(), change.index_id, *change.old_key);
                        }
                        if (change.new_key) {
                            versions->RegisterIndexWrite(context->GetTransaction(),
                                update.GetTableId(), change.index_id, *change.new_key);
                        }
                    }
                }
            }
            for (const auto& replacement : replacements) {
                for (const auto& change : replacement.index_changes) {
                    if (!change.new_key || !catalog_.GetIndex(change.index_id).GetTree().IsUnique()) { continue; }
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
                if (versions) {
                    versions->AppendUndoRecord(context->GetTransaction(), replacement.rid,
                                               replacement.old_record,
                                               replacement.old_meta);
                }
                if (!heap.UpdateRecord(replacement.rid, replacement.record)) {
                    throw std::runtime_error("Updated record does not fit in its current page");
                }
                if (versions) {
                    heap.SetTupleMeta(
                        replacement.rid,
                        {TransactionManager::EncodeTransactionTimestamp(
                             context->GetTransaction().GetId()), false});
                    versions->RegisterWrite(context->GetTransaction(), replacement.rid);
                }
                for (const auto& change : replacement.index_changes) {
                    auto& tree = catalog_.GetIndex(change.index_id).GetTree();
                    if (versions && change.old_key) {
                        versions->RegisterStaleIndexEntry(context->GetTransaction(),
                                                          change.index_id, *change.old_key,
                                                          change.rid);
                    }
                    if (change.old_key && !tree.Remove(*change.old_key, change.rid)) {
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

ExecutionResult Executor::Execute(const PlanNode& plan, ExecutionContext& context) {
    auto& transaction = context.GetTransaction();
    if (!transaction.IsActive()) { throw std::logic_error("Execution transaction is not active"); }
    StatementLockGuard statement_locks(context);
    AcquireExecutionLocks(plan, statement_locks, catalog_);
    RegisterSerializablePlanReads(plan, context);
    auto& pool = catalog_.GetBufferPoolManager();
    const bool writes_pages = WritesPages(plan.GetType());
    if (writes_pages) { pool.SetActiveTransaction(&transaction); }
    struct ActiveTransactionReset {
        BufferPoolManager& pool;
        bool active;
        ~ActiveTransactionReset() { if (active) { pool.SetActiveTransaction(nullptr); } }
    } reset{pool, writes_pages};
    auto result = ExecutePlan(plan, &context);
    pool.ThrowIfWriteError();
    result.transaction_id = transaction.GetId();
    return result;
}

}  // namespace udb::sql
