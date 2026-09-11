#pragma once

#include "udb/table_metadata.h"
#include "udb/sql/bound_expression.h"
#include "udb/value.h"

#include <memory>

namespace udb::sql {

enum class PlanType { CreateTable, Insert, SeqScan };

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
                BoundExpressionPtr predicate = nullptr)
        : PlanNode(PlanType::SeqScan, std::move(output_schema)), table_id_(id),
          indexes_(std::move(indexes)), predicate_(std::move(predicate)) {}
    table_id_t GetTableId() const { return table_id_; }
    const std::vector<std::size_t>& GetColumnIndexes() const { return indexes_; }
    const BoundExpressionPtr& GetPredicate() const { return predicate_; }

private:
    table_id_t table_id_;
    std::vector<std::size_t> indexes_;
    BoundExpressionPtr predicate_;
};

}  // namespace udb::sql
