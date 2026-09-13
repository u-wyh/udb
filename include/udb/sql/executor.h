#pragma once

#include "udb/catalog.h"
#include "udb/sql/plan.h"
#include "udb/tuple.h"
#include "udb/execution_context.h"

namespace udb::sql {

// A returned result denotes success; errors propagate as exceptions.
// CREATE/DROP/INSERT/DELETE/UPDATE have empty output schemas and rows. Mutations report
// their affected row count.
struct ExecutionResult {
    explicit ExecutionResult(PlanType plan_type) : type(plan_type) {}
    PlanType type;
    std::size_t affected_rows = 0;
    Schema output_schema{std::vector<Column>{}};
    std::vector<Tuple> rows;
    std::optional<RID> inserted_rid;
    std::optional<transaction_id_t> transaction_id;
};

// Catalog must outlive this executor. Execute consumes already planned inputs;
// no parsing, binding, or automatic flush. SELECT materializes all output rows.
class Executor {
public:
    explicit Executor(Catalog& catalog) : catalog_(catalog) {}
    ExecutionResult Execute(const PlanNode& plan);
    ExecutionResult Execute(const PlanNode& plan, ExecutionContext& context);

private:
    ExecutionResult ExecutePlan(const PlanNode& plan, ExecutionContext* context);
    Catalog& catalog_;
};

}  // namespace udb::sql
