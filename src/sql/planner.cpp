#include "udb/sql/planner.h"

#include <optional>
#include <type_traits>

namespace udb::sql {
namespace {

struct EqualityCandidate {
    std::size_t column_index;
    std::int64_t key;
};

void CollectEqualityCandidates(const BoundExpressionPtr& expression,
                               std::vector<EqualityCandidate>& candidates) {
    if (!expression) { return; }
    if (const auto* logical = std::get_if<BoundLogicalExpression>(&expression->node)) {
        if (logical->op == LogicalOperator::And) {
            CollectEqualityCandidates(logical->left, candidates);
            CollectEqualityCandidates(logical->right, candidates);
        }
        return;
    }
    const auto* comparison = std::get_if<BoundComparisonExpression>(&expression->node);
    if (comparison == nullptr || comparison->op != ComparisonOperator::Equal ||
        !comparison->left || !comparison->right) {
        return;
    }
    const BoundColumnExpression* column =
        std::get_if<BoundColumnExpression>(&comparison->left->node);
    const BoundLiteralExpression* literal =
        std::get_if<BoundLiteralExpression>(&comparison->right->node);
    if (column == nullptr || literal == nullptr) {
        column = std::get_if<BoundColumnExpression>(&comparison->right->node);
        literal = std::get_if<BoundLiteralExpression>(&comparison->left->node);
    }
    if (column == nullptr || literal == nullptr || literal->value.IsNull()) { return; }
    if (literal->value.GetType() == TypeId::INTEGER) {
        candidates.push_back({column->column_index,
                              static_cast<std::int64_t>(literal->value.GetInteger())});
    } else if (literal->value.GetType() == TypeId::BIGINT) {
        candidates.push_back({column->column_index, literal->value.GetBigInt()});
    }
}

std::unique_ptr<PlanNode> Build(const BoundCreateTableStatement& statement) {
    return std::make_unique<CreateTablePlan>(statement.table_name, statement.schema);
}

std::unique_ptr<PlanNode> Build(const BoundCreateIndexStatement& statement) {
    return std::make_unique<CreateIndexPlan>(statement.index_name, statement.table_id,
                                              statement.column_index);
}

std::unique_ptr<PlanNode> Build(const BoundDropTableStatement& statement) {
    return std::make_unique<DropTablePlan>(statement.table_id);
}

std::unique_ptr<PlanNode> Build(const BoundInsertStatement& statement) {
    return std::make_unique<InsertPlan>(statement.table_id, statement.schema, statement.values);
}

std::unique_ptr<PlanNode> Build(const BoundSelectStatement& statement) {
    return std::make_unique<SeqScanPlan>(statement.table_id, statement.column_indexes,
                                         statement.output_schema, statement.predicate);
}

std::unique_ptr<PlanNode> Build(const BoundSelectStatement& statement,
                                const Catalog& catalog) {
    std::vector<EqualityCandidate> candidates;
    CollectEqualityCandidates(statement.predicate, candidates);
    std::optional<index_id_t> selected_index;
    std::optional<std::int64_t> selected_key;
    for (const auto& candidate : candidates) {
        for (const auto index_id : catalog.GetTableIndexes(statement.table_id)) {
            const auto& metadata = catalog.GetIndex(index_id).GetMetadata();
            if (metadata.GetColumnIndex() == candidate.column_index &&
                (!selected_index || index_id < *selected_index)) {
                selected_index = index_id;
                selected_key = candidate.key;
            }
        }
    }
    if (!selected_index) { return Build(statement); }
    return std::make_unique<IndexScanPlan>(statement.table_id, *selected_index,
                                           *selected_key, statement.column_indexes,
                                           statement.output_schema, statement.predicate);
}

std::unique_ptr<PlanNode> Build(const BoundDeleteStatement& statement) {
    return std::make_unique<DeletePlan>(statement.table_id, statement.schema, statement.predicate);
}

std::unique_ptr<PlanNode> Build(const BoundUpdateStatement& statement) {
    std::vector<UpdatePlanAssignment> assignments;
    assignments.reserve(statement.assignments.size());
    for (const auto& assignment : statement.assignments) {
        assignments.push_back({assignment.column_index, assignment.value});
    }
    return std::make_unique<UpdatePlan>(statement.table_id, statement.schema,
                                        std::move(assignments), statement.predicate);
}

}  // namespace

std::unique_ptr<PlanNode> Planner::Plan(const BoundStatement& statement) {
    return std::visit([](const auto& bound) { return Build(bound); }, statement);
}

std::unique_ptr<PlanNode> Planner::Plan(const BoundStatement& statement,
                                        const Catalog& catalog) {
    return std::visit([&catalog](const auto& bound) -> std::unique_ptr<PlanNode> {
        using Bound = std::decay_t<decltype(bound)>;
        if constexpr (std::is_same_v<Bound, BoundSelectStatement>) {
            return Build(bound, catalog);
        }
        return Build(bound);
    }, statement);
}

}  // namespace udb::sql
