#include "udb/sql/planner.h"

namespace udb::sql {
namespace {

std::unique_ptr<PlanNode> Build(const BoundCreateTableStatement& statement) {
    return std::make_unique<CreateTablePlan>(statement.table_name, statement.schema);
}

std::unique_ptr<PlanNode> Build(const BoundInsertStatement& statement) {
    return std::make_unique<InsertPlan>(statement.table_id, statement.schema, statement.values);
}

std::unique_ptr<PlanNode> Build(const BoundSelectStatement& statement) {
    return std::make_unique<SeqScanPlan>(statement.table_id, statement.column_indexes, statement.output_schema);
}

}  // namespace

std::unique_ptr<PlanNode> Planner::Plan(const BoundStatement& statement) {
    return std::visit([](const auto& bound) { return Build(bound); }, statement);
}

}  // namespace udb::sql
