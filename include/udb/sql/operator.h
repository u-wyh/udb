#pragma once

#include "udb/sql/bound_expression.h"
#include "udb/table_heap.h"

namespace udb::sql {

// Operators consume already bound inputs. This first interface materializes
// batches; each pipeline is owned by one Execute call, with no implicit I/O flush.
class ExecutionOperator {
public:
    virtual ~ExecutionOperator() = default;
    virtual std::vector<Tuple> Execute() = 0;
};

class MaterializedOperator final : public ExecutionOperator {
public:
    explicit MaterializedOperator(std::vector<Tuple> rows) : rows_(std::move(rows)) {}
    std::vector<Tuple> Execute() override { return std::move(rows_); }
private:
    std::vector<Tuple> rows_;
};

class TableScanOperator final : public ExecutionOperator {
public:
    TableScanOperator(const TableHeap& heap, Schema schema) : heap_(heap), schema_(std::move(schema)) {}
    std::vector<Tuple> Execute() override;
private:
    const TableHeap& heap_;
    Schema schema_;
};

class FilterOperator final : public ExecutionOperator {
public:
    FilterOperator(std::unique_ptr<ExecutionOperator> input, BoundExpressionPtr predicate);
    std::vector<Tuple> Execute() override;
    static bool Matches(const BoundExpressionPtr& predicate, const Tuple& tuple);
private:
    std::unique_ptr<ExecutionOperator> input_;
    BoundExpressionPtr predicate_;
};

class ProjectionOperator final : public ExecutionOperator {
public:
    ProjectionOperator(std::unique_ptr<ExecutionOperator> input, Schema output,
                       std::vector<std::size_t> indexes, std::vector<BoundExpressionPtr> expressions);
    std::vector<Tuple> Execute() override;
private:
    std::unique_ptr<ExecutionOperator> input_;
    Schema output_;
    std::vector<std::size_t> indexes_;
    std::vector<BoundExpressionPtr> expressions_;
};

}  // namespace udb::sql
