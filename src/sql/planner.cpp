#include "udb/sql/planner.h"

#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace udb::sql {
namespace {

struct EqualityCandidate {
    std::size_t column_index;
    std::int64_t key;
};

struct RangeCandidate {
    std::optional<std::int64_t> lower;
    std::optional<std::int64_t> upper;
    bool lower_inclusive = false;
    bool upper_inclusive = false;
};

ComparisonOperator Reverse(ComparisonOperator op) {
    switch (op) {
        case ComparisonOperator::Equal: return ComparisonOperator::Equal;
        case ComparisonOperator::NotEqual: return ComparisonOperator::NotEqual;
        case ComparisonOperator::Less: return ComparisonOperator::Greater;
        case ComparisonOperator::LessEqual: return ComparisonOperator::GreaterEqual;
        case ComparisonOperator::Greater: return ComparisonOperator::Less;
        case ComparisonOperator::GreaterEqual: return ComparisonOperator::LessEqual;
    }
    throw std::logic_error("Unknown comparison operator");
}

void SetLowerBound(RangeCandidate& range, std::int64_t key, bool inclusive) {
    if (!range.lower || key > *range.lower ||
        (key == *range.lower && !inclusive && range.lower_inclusive)) {
        range.lower = key;
        range.lower_inclusive = inclusive;
    }
}

void SetUpperBound(RangeCandidate& range, std::int64_t key, bool inclusive) {
    if (!range.upper || key < *range.upper ||
        (key == *range.upper && !inclusive && range.upper_inclusive)) {
        range.upper = key;
        range.upper_inclusive = inclusive;
    }
}

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

void CollectRangeCandidates(const BoundExpressionPtr& expression,
                            std::map<std::size_t, RangeCandidate>& candidates) {
    if (!expression) { return; }
    if (const auto* logical = std::get_if<BoundLogicalExpression>(&expression->node)) {
        if (logical->op == LogicalOperator::And) {
            CollectRangeCandidates(logical->left, candidates);
            CollectRangeCandidates(logical->right, candidates);
        }
        return;
    }
    const auto* comparison = std::get_if<BoundComparisonExpression>(&expression->node);
    if (comparison == nullptr || !comparison->left || !comparison->right) { return; }
    const BoundColumnExpression* column =
        std::get_if<BoundColumnExpression>(&comparison->left->node);
    const BoundLiteralExpression* literal =
        std::get_if<BoundLiteralExpression>(&comparison->right->node);
    auto op = comparison->op;
    if (column == nullptr || literal == nullptr) {
        column = std::get_if<BoundColumnExpression>(&comparison->right->node);
        literal = std::get_if<BoundLiteralExpression>(&comparison->left->node);
        op = Reverse(op);
    }
    if (column == nullptr || literal == nullptr || literal->value.IsNull() ||
        op == ComparisonOperator::Equal || op == ComparisonOperator::NotEqual) {
        return;
    }
    std::int64_t key;
    if (literal->value.GetType() == TypeId::INTEGER) {
        key = literal->value.GetInteger();
    } else if (literal->value.GetType() == TypeId::BIGINT) {
        key = literal->value.GetBigInt();
    } else {
        return;
    }
    auto& range = candidates[column->column_index];
    if (op == ComparisonOperator::Greater || op == ComparisonOperator::GreaterEqual) {
        SetLowerBound(range, key, op == ComparisonOperator::GreaterEqual);
    } else {
        SetUpperBound(range, key, op == ComparisonOperator::LessEqual);
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
    const auto table_indexes = catalog.GetTableIndexes(statement.table_id);
    for (const auto& candidate : candidates) {
        for (const auto index_id : table_indexes) {
            const auto& metadata = catalog.GetIndex(index_id).GetMetadata();
            if (metadata.GetColumnIndex() == candidate.column_index &&
                (!selected_index || index_id < *selected_index)) {
                selected_index = index_id;
                selected_key = candidate.key;
            }
        }
    }
    if (selected_index) {
        return std::make_unique<IndexScanPlan>(statement.table_id, *selected_index,
                                               *selected_key, statement.column_indexes,
                                               statement.output_schema, statement.predicate);
    }

    std::map<std::size_t, RangeCandidate> ranges;
    CollectRangeCandidates(statement.predicate, ranges);
    std::optional<RangeCandidate> selected_range;
    for (const auto& [column_index, range] : ranges) {
        for (const auto index_id : table_indexes) {
            const auto& metadata = catalog.GetIndex(index_id).GetMetadata();
            if (metadata.GetColumnIndex() == column_index &&
                (!selected_index || index_id < *selected_index)) {
                selected_index = index_id;
                selected_range = range;
            }
        }
    }
    if (!selected_index) { return Build(statement); }
    return std::make_unique<IndexRangeScanPlan>(
        statement.table_id, *selected_index, selected_range->lower,
        selected_range->lower_inclusive, selected_range->upper,
        selected_range->upper_inclusive, statement.column_indexes,
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
