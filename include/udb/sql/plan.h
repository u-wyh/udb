#pragma once

#include "udb/table_metadata.h"
#include "udb/index_metadata.h"
#include "udb/sql/bound_expression.h"
#include "udb/value.h"

#include <memory>
#include <optional>

namespace udb::sql {

enum class PlanType {
    CreateTable, CreateIndex, DropTable, DropIndex, Insert, SeqScan, IndexScan,
    IndexRangeScan, Delete, Update
};

struct PlanOrderBy {
    std::size_t column_index;
    bool ascending;
};

// Logical descriptions only. All current plans are leaves with no children.
class PlanNode {
public:
    virtual ~PlanNode() = default;
    PlanType GetType() const { return type_; }
    const Schema& GetOutputSchema() const { return output_schema_; }
    const std::vector<std::unique_ptr<PlanNode>>& GetChildren() const { return children_; }

protected:
    PlanNode(PlanType type, Schema output_schema)
        : type_(type), output_schema_(std::move(output_schema)) {}

private:
    PlanType type_;
    Schema output_schema_;
    std::vector<std::unique_ptr<PlanNode>> children_;
};

class CreateTablePlan final : public PlanNode {
public:
    CreateTablePlan(std::string name, Schema schema)
        : PlanNode(PlanType::CreateTable, Schema({})), table_name_(std::move(name)), schema_(std::move(schema)) {}
    const std::string& GetTableName() const { return table_name_; }
    const Schema& GetTableSchema() const { return schema_; }

private:
    std::string table_name_;
    Schema schema_;
};

class CreateIndexPlan final : public PlanNode {
public:
    CreateIndexPlan(std::string name, table_id_t table_id, std::size_t column_index)
        : PlanNode(PlanType::CreateIndex, Schema({})), index_name_(std::move(name)),
          table_id_(table_id), column_index_(column_index) {}
    const std::string& GetIndexName() const { return index_name_; }
    table_id_t GetTableId() const { return table_id_; }
    std::size_t GetColumnIndex() const { return column_index_; }

private:
    std::string index_name_;
    table_id_t table_id_;
    std::size_t column_index_;
};

class DropTablePlan final : public PlanNode {
public:
    explicit DropTablePlan(table_id_t id)
        : PlanNode(PlanType::DropTable, Schema({})), table_id_(id) {}
    table_id_t GetTableId() const { return table_id_; }

private:
    table_id_t table_id_;
};

class DropIndexPlan final : public PlanNode {
public:
    explicit DropIndexPlan(index_id_t id)
        : PlanNode(PlanType::DropIndex, Schema({})), index_id_(id) {}
    index_id_t GetIndexId() const { return index_id_; }

private:
    index_id_t index_id_;
};

class InsertPlan final : public PlanNode {
public:
    InsertPlan(table_id_t id, Schema schema, std::vector<Value> values)
        : PlanNode(PlanType::Insert, Schema({})), table_id_(id), schema_(std::move(schema)), values_(std::move(values)) {}
    table_id_t GetTableId() const { return table_id_; }
    const Schema& GetTableSchema() const { return schema_; }
    const std::vector<Value>& GetValues() const { return values_; }

private:
    table_id_t table_id_;
    Schema schema_;
    std::vector<Value> values_;
};

class SeqScanPlan final : public PlanNode {
public:
    SeqScanPlan(table_id_t id, std::vector<std::size_t> indexes, Schema output_schema,
                BoundExpressionPtr predicate = nullptr,
                std::vector<PlanOrderBy> order_by = {},
                std::optional<std::size_t> limit = std::nullopt,
                std::size_t offset = 0)
        : PlanNode(PlanType::SeqScan, std::move(output_schema)), table_id_(id),
          indexes_(std::move(indexes)), predicate_(std::move(predicate)),
          order_by_(order_by), limit_(limit), offset_(offset) {}
    table_id_t GetTableId() const { return table_id_; }
    const std::vector<std::size_t>& GetColumnIndexes() const { return indexes_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }
    const std::vector<PlanOrderBy>& GetOrderBy() const { return order_by_; }
    const std::optional<std::size_t>& GetLimit() const { return limit_; }
    std::size_t GetOffset() const { return offset_; }

private:
    table_id_t table_id_;
    std::vector<std::size_t> indexes_;
    BoundExpressionPtr predicate_;
    std::vector<PlanOrderBy> order_by_;
    std::optional<std::size_t> limit_;
    std::size_t offset_;
};

class IndexScanPlan final : public PlanNode {
public:
    IndexScanPlan(table_id_t table_id, index_id_t index_id, std::int64_t key,
                  std::vector<std::size_t> indexes, Schema output_schema,
                  BoundExpressionPtr predicate,
                  std::vector<PlanOrderBy> order_by = {},
                  std::optional<std::size_t> limit = std::nullopt,
                  std::size_t offset = 0)
        : PlanNode(PlanType::IndexScan, std::move(output_schema)), table_id_(table_id),
          index_id_(index_id), key_(key), indexes_(std::move(indexes)),
          predicate_(std::move(predicate)), order_by_(order_by), limit_(limit), offset_(offset) {}
    table_id_t GetTableId() const { return table_id_; }
    index_id_t GetIndexId() const { return index_id_; }
    std::int64_t GetKey() const { return key_; }
    const std::vector<std::size_t>& GetColumnIndexes() const { return indexes_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }
    const std::vector<PlanOrderBy>& GetOrderBy() const { return order_by_; }
    const std::optional<std::size_t>& GetLimit() const { return limit_; }
    std::size_t GetOffset() const { return offset_; }

private:
    table_id_t table_id_;
    index_id_t index_id_;
    std::int64_t key_;
    std::vector<std::size_t> indexes_;
    BoundExpressionPtr predicate_;
    std::vector<PlanOrderBy> order_by_;
    std::optional<std::size_t> limit_;
    std::size_t offset_;
};

class IndexRangeScanPlan final : public PlanNode {
public:
    IndexRangeScanPlan(table_id_t table_id, index_id_t index_id,
                       std::optional<std::int64_t> lower, bool lower_inclusive,
                       std::optional<std::int64_t> upper, bool upper_inclusive,
                       std::vector<std::size_t> indexes, Schema output_schema,
                       BoundExpressionPtr predicate,
                       std::vector<PlanOrderBy> order_by = {},
                       std::optional<std::size_t> limit = std::nullopt,
                       std::size_t offset = 0)
        : PlanNode(PlanType::IndexRangeScan, std::move(output_schema)), table_id_(table_id),
          index_id_(index_id), lower_(lower), upper_(upper),
          lower_inclusive_(lower_inclusive), upper_inclusive_(upper_inclusive),
          indexes_(std::move(indexes)), predicate_(std::move(predicate)),
          order_by_(order_by), limit_(limit), offset_(offset) {}
    table_id_t GetTableId() const { return table_id_; }
    index_id_t GetIndexId() const { return index_id_; }
    const std::optional<std::int64_t>& GetLowerBound() const { return lower_; }
    const std::optional<std::int64_t>& GetUpperBound() const { return upper_; }
    bool IsLowerInclusive() const { return lower_inclusive_; }
    bool IsUpperInclusive() const { return upper_inclusive_; }
    const std::vector<std::size_t>& GetColumnIndexes() const { return indexes_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }
    const std::vector<PlanOrderBy>& GetOrderBy() const { return order_by_; }
    const std::optional<std::size_t>& GetLimit() const { return limit_; }
    std::size_t GetOffset() const { return offset_; }

private:
    table_id_t table_id_;
    index_id_t index_id_;
    std::optional<std::int64_t> lower_;
    std::optional<std::int64_t> upper_;
    bool lower_inclusive_;
    bool upper_inclusive_;
    std::vector<std::size_t> indexes_;
    BoundExpressionPtr predicate_;
    std::vector<PlanOrderBy> order_by_;
    std::optional<std::size_t> limit_;
    std::size_t offset_;
};

class DeletePlan final : public PlanNode {
public:
    DeletePlan(table_id_t id, Schema schema, BoundExpressionPtr predicate = nullptr)
        : PlanNode(PlanType::Delete, Schema({})), table_id_(id),
          schema_(std::move(schema)), predicate_(std::move(predicate)) {}
    table_id_t GetTableId() const { return table_id_; }
    const Schema& GetTableSchema() const { return schema_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }

private:
    table_id_t table_id_;
    Schema schema_;
    BoundExpressionPtr predicate_;
};

struct UpdatePlanAssignment {
    std::size_t column_index;
    Value value;
};

class UpdatePlan final : public PlanNode {
public:
    UpdatePlan(table_id_t id, Schema schema, std::vector<UpdatePlanAssignment> assignments,
               BoundExpressionPtr predicate = nullptr)
        : PlanNode(PlanType::Update, Schema({})), table_id_(id), schema_(std::move(schema)),
          assignments_(std::move(assignments)), predicate_(std::move(predicate)) {}
    table_id_t GetTableId() const { return table_id_; }
    const Schema& GetTableSchema() const { return schema_; }
    const std::vector<UpdatePlanAssignment>& GetAssignments() const { return assignments_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }

private:
    table_id_t table_id_;
    Schema schema_;
    std::vector<UpdatePlanAssignment> assignments_;
    BoundExpressionPtr predicate_;
};

}  // namespace udb::sql
