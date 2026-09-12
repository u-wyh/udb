#include "udb/sql/planner.h"
#include "udb/sql/cost_model.h"

#include <map>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace udb::sql {
namespace {

struct EqualityCandidate {
    std::size_t column_index;
    IndexKey key;
};

struct RangeCandidate {
    std::optional<IndexKey> lower;
    std::optional<IndexKey> upper;
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

void SetLowerBound(RangeCandidate& range, IndexKey key, bool inclusive) {
    if (!range.lower || key > *range.lower ||
        (key == *range.lower && !inclusive && range.lower_inclusive)) {
        range.lower = key;
        range.lower_inclusive = inclusive;
    }
}

void SetUpperBound(RangeCandidate& range, IndexKey key, bool inclusive) {
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
    } else if (literal->value.GetType() == TypeId::VARCHAR) {
        candidates.push_back({column->column_index, IndexKey(literal->value.GetVarchar())});
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
    IndexKey key;
    if (literal->value.GetType() == TypeId::INTEGER) {
        key = literal->value.GetInteger();
    } else if (literal->value.GetType() == TypeId::BIGINT) {
        key = literal->value.GetBigInt();
    } else if (literal->value.GetType() == TypeId::VARCHAR) {
        key = IndexKey(literal->value.GetVarchar());
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

bool PredicateCoveredByColumn(const BoundExpressionPtr& expression, std::size_t column_index) {
    if (!expression) { return false; }
    if (const auto* logical = std::get_if<BoundLogicalExpression>(&expression->node)) {
        return logical->op == LogicalOperator::And &&
            PredicateCoveredByColumn(logical->left, column_index) &&
            PredicateCoveredByColumn(logical->right, column_index);
    }
    const auto* comparison = std::get_if<BoundComparisonExpression>(&expression->node);
    if (!comparison || comparison->op == ComparisonOperator::NotEqual ||
        !comparison->left || !comparison->right) { return false; }
    const auto* left_column = std::get_if<BoundColumnExpression>(&comparison->left->node);
    const auto* right_column = std::get_if<BoundColumnExpression>(&comparison->right->node);
    const auto* left_literal = std::get_if<BoundLiteralExpression>(&comparison->left->node);
    const auto* right_literal = std::get_if<BoundLiteralExpression>(&comparison->right->node);
    return ((left_column && right_literal && !right_literal->value.IsNull() &&
             left_column->column_index == column_index) ||
            (right_column && left_literal && !left_literal->value.IsNull() &&
             right_column->column_index == column_index));
}

bool ProjectionCoveredByColumn(const BoundSelectStatement& statement, std::size_t column_index) {
    if (!statement.order_by.empty() || statement.output_schema.GetColumnCount() != 1) { return false; }
    if (statement.projections.empty()) {
        return statement.column_indexes.size() == 1 && statement.column_indexes[0] == column_index;
    }
    if (statement.projections.size() != 1 || !statement.column_indexes.empty()) { return false; }
    const auto* column = statement.projections[0]
        ? std::get_if<BoundColumnExpression>(&statement.projections[0]->node) : nullptr;
    return column && column->column_index == column_index;
}

std::unique_ptr<PlanNode> Build(const BoundCreateTableStatement& statement) {
    return std::make_unique<CreateTablePlan>(statement.table_name, statement.schema);
}

std::unique_ptr<PlanNode> Build(const BoundCreateIndexStatement& statement) {
    return std::make_unique<CreateIndexPlan>(statement.index_name, statement.table_id,
                                              statement.column_index, statement.column_indexes);
}

std::unique_ptr<PlanNode> Build(const BoundDropTableStatement& statement) {
    return std::make_unique<DropTablePlan>(statement.table_id);
}

std::unique_ptr<PlanNode> Build(const BoundDropIndexStatement& statement) {
    return std::make_unique<DropIndexPlan>(statement.index_id);
}

std::unique_ptr<PlanNode> Build(const BoundInsertStatement& statement) {
    return std::make_unique<InsertPlan>(statement.table_id, statement.schema, statement.values);
}

std::unique_ptr<PlanNode> Build(const BoundSelectStatement& statement,
                              JoinAlgorithm algorithm = JoinAlgorithm::NestedLoop,
                              std::optional<bool> smaller_input_is_left = std::nullopt) {
    if (statement.second_table_id) {
        std::vector<PlanOrderBy> order_by;
        for (const auto& order : statement.order_by) {
            order_by.push_back({order.column_index, order.ascending});
        }
        return std::make_unique<JoinPlan>(statement.table_id, *statement.second_table_id,
            statement.column_indexes, statement.output_schema, statement.predicate,
            std::move(order_by), statement.limit, statement.offset, statement.projections,
            statement.join_condition, algorithm,
            smaller_input_is_left.value_or(algorithm != JoinAlgorithm::Hash));
    }
    if (!statement.aggregates.empty()) {
        std::vector<PlanAggregate> aggregates;
        for (const auto& aggregate : statement.aggregates) {
            aggregates.push_back({aggregate.type, aggregate.column_index, aggregate.input_type});
        }
        return std::make_unique<AggregatePlan>(statement.table_id, std::move(aggregates),
            statement.output_schema, statement.predicate, statement.limit, statement.offset,
            statement.group_by_column, statement.project_group_by, statement.having);
    }
    std::vector<PlanOrderBy> order_by;
    for (const auto& order : statement.order_by) {
        order_by.push_back({order.column_index, order.ascending});
    }
    return std::make_unique<SeqScanPlan>(statement.table_id, statement.column_indexes,
                                         statement.output_schema, statement.predicate,
                                         order_by, statement.limit, statement.offset,
                                         statement.projections);
}

std::unique_ptr<PlanNode> Build(const BoundSelectStatement& statement,
                                const Catalog& catalog) {
    if (statement.second_table_id) {
        // Bound column equality needs no row counts or string lookup. Keep the
        // catalog-free planner as a deterministic nested-loop reference path.
        const auto* equality = statement.join_condition
            ? std::get_if<BoundComparisonExpression>(&statement.join_condition->node) : nullptr;
        const bool hashable = equality && equality->op == ComparisonOperator::Equal &&
            equality->left && equality->right &&
            std::holds_alternative<BoundColumnExpression>(equality->left->node) &&
            std::holds_alternative<BoundColumnExpression>(equality->right->node) &&
            equality->left->type == equality->right->type && equality->left->type != TypeId::DOUBLE;
        const auto algorithm = hashable ? JoinAlgorithm::Hash : JoinAlgorithm::NestedLoop;
        std::optional<bool> smaller_input_is_left;
        if (catalog.HasTableStatistics(statement.table_id) &&
            catalog.HasTableStatistics(*statement.second_table_id)) {
            smaller_input_is_left = catalog.GetTableStatistics(statement.table_id).row_count <=
                                    catalog.GetTableStatistics(*statement.second_table_id).row_count;
        }
        return Build(statement, algorithm, smaller_input_is_left);
    }
    if (!statement.aggregates.empty()) { return Build(statement); }
    std::vector<PlanOrderBy> order_by;
    for (const auto& order : statement.order_by) {
        order_by.push_back({order.column_index, order.ascending});
    }
    std::vector<EqualityCandidate> candidates;
    CollectEqualityCandidates(statement.predicate, candidates);
    const auto table_indexes = catalog.GetTableIndexes(statement.table_id);
    auto best = Build(statement);
    auto best_cost = CostModel::Estimate(*best, catalog).total_cost;
    std::size_t best_coverage = 0;
    const auto consider = [&](std::unique_ptr<PlanNode> plan, std::size_t coverage) {
        const auto cost = CostModel::Estimate(*plan, catalog).total_cost;
        if (cost < best_cost || (cost == best_cost && coverage > best_coverage)) {
            best = std::move(plan);
            best_cost = cost;
            best_coverage = coverage;
        }
    };

    for (const auto index_id : table_indexes) {
        const auto& index = catalog.GetIndex(index_id);
        const auto& metadata = index.GetMetadata();
        std::vector<IndexKey> components;
        for (const auto column : metadata.GetColumnIndexes()) {
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                [column](const EqualityCandidate& candidate) { return candidate.column_index == column; });
            if (found == candidates.end()) { break; }
            components.push_back(found->key);
        }
        if (components.size() != metadata.GetColumnIndexes().size()) { continue; }
        auto key = components.size() == 1 ? components[0] : MakeCompositeKey(components);
        if (key.IsString() && key.GetString().size() > index.GetTree().GetStringMaxLength()) { continue; }
        if (metadata.GetColumnIndexes().size() == 1 &&
            ProjectionCoveredByColumn(statement, metadata.GetColumnIndex()) &&
            PredicateCoveredByColumn(statement.predicate, metadata.GetColumnIndex())) {
            consider(std::make_unique<IndexOnlyScanPlan>(
                statement.table_id, index_id, key,
                std::nullopt, false, std::nullopt, false, metadata.GetColumnIndex(),
                statement.output_schema, statement.predicate, statement.limit, statement.offset),
                metadata.GetColumnIndexes().size());
        } else {
            consider(std::make_unique<IndexScanPlan>(
                statement.table_id, index_id, key, statement.column_indexes,
                statement.output_schema, statement.predicate, order_by,
                statement.limit, statement.offset, statement.projections),
                metadata.GetColumnIndexes().size());
        }
    }

    std::map<std::size_t, RangeCandidate> ranges;
    CollectRangeCandidates(statement.predicate, ranges);
    for (const auto& [column_index, range] : ranges) {
        for (const auto index_id : table_indexes) {
            const auto& index = catalog.GetIndex(index_id);
            const auto& metadata = index.GetMetadata();
            const auto max_length = index.GetTree().GetStringMaxLength();
            if ((range.lower && range.lower->IsString() && range.lower->GetString().size() > max_length) ||
                (range.upper && range.upper->IsString() && range.upper->GetString().size() > max_length)) { continue; }
            if (metadata.GetColumnIndexes().size() != 1 || metadata.GetColumnIndex() != column_index) { continue; }
            if (ProjectionCoveredByColumn(statement, column_index) &&
                PredicateCoveredByColumn(statement.predicate, column_index)) {
                consider(std::make_unique<IndexOnlyScanPlan>(
                    statement.table_id, index_id, std::nullopt, range.lower,
                    range.lower_inclusive, range.upper, range.upper_inclusive,
                    column_index, statement.output_schema, statement.predicate,
                    statement.limit, statement.offset), 1);
            } else {
                consider(std::make_unique<IndexRangeScanPlan>(
                    statement.table_id, index_id, range.lower, range.lower_inclusive,
                    range.upper, range.upper_inclusive, statement.column_indexes,
                    statement.output_schema, statement.predicate, order_by,
                    statement.limit, statement.offset, statement.projections), 1);
            }
        }
    }
    return best;
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
