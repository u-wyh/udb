#pragma once

#include "udb/sql/bound_expression.h"
#include "udb/table_heap.h"

namespace udb::sql {

// Init resets an operator; Next returns one row or end-of-stream. Operators
// retain no page pins between calls. Execute is a materializing compatibility
// adapter for the public ExecutionResult API, not an intermediate batch API.
class ExecutionOperator {
public:
    virtual ~ExecutionOperator() = default;
    virtual void Init() = 0;
    virtual std::optional<Tuple> Next() = 0;
    std::vector<Tuple> Execute();
protected:
    void RequireInitialized() const {
        if (!initialized_) { throw std::logic_error("Operator must be initialized"); }
    }
    bool initialized_ = false;
};

class MaterializedOperator final : public ExecutionOperator {
public:
    explicit MaterializedOperator(std::vector<Tuple> rows) : rows_(std::move(rows)) {}
    void Init() override { position_ = 0; initialized_ = true; }
    std::optional<Tuple> Next() override {
        RequireInitialized();
        if (position_ == rows_.size()) { return std::nullopt; }
        return rows_[position_++];
    }
private:
    std::vector<Tuple> rows_;
    std::size_t position_ = 0;
};

class TableScanOperator final : public ExecutionOperator {
public:
    TableScanOperator(const TableHeap& heap, Schema schema) : heap_(heap), schema_(std::move(schema)) {}
    void Init() override { current_.reset(); started_ = false; ended_ = false; initialized_ = true; }
    std::optional<Tuple> Next() override;
private:
    const TableHeap& heap_;
    Schema schema_;
    std::optional<RID> current_;
    bool started_ = false;
    bool ended_ = false;
};

class FilterOperator final : public ExecutionOperator {
public:
    FilterOperator(std::unique_ptr<ExecutionOperator> input, BoundExpressionPtr predicate);
    void Init() override { input_->Init(); initialized_ = true; }
    std::optional<Tuple> Next() override;
    static bool Matches(const BoundExpressionPtr& predicate, const Tuple& tuple);
private:
    std::unique_ptr<ExecutionOperator> input_;
    BoundExpressionPtr predicate_;
};

class ProjectionOperator final : public ExecutionOperator {
public:
    ProjectionOperator(std::unique_ptr<ExecutionOperator> input, Schema output,
                       std::vector<std::size_t> indexes, std::vector<BoundExpressionPtr> expressions);
    void Init() override { input_->Init(); initialized_ = true; }
    std::optional<Tuple> Next() override;
private:
    std::unique_ptr<ExecutionOperator> input_;
    Schema output_;
    std::vector<std::size_t> indexes_;
    std::vector<BoundExpressionPtr> expressions_;
};

class LimitOperator final : public ExecutionOperator {
public:
    LimitOperator(std::unique_ptr<ExecutionOperator> input,
                  std::optional<std::size_t> limit, std::size_t offset);
    void Init() override;
    std::optional<Tuple> Next() override;
private:
    std::unique_ptr<ExecutionOperator> input_;
    std::optional<std::size_t> limit_;
    std::size_t offset_;
    std::size_t skipped_ = 0;
    std::size_t emitted_ = 0;
    bool ended_ = false;
};

}  // namespace udb::sql
