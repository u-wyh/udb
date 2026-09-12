#pragma once

#include "udb/sql/bound_statement.h"
#include "udb/sql/plan.h"
#include "udb/catalog.h"

namespace udb::sql {

class Planner {
public:
    // Catalog-free planning preserves the original bound-to-plan conversion and
    // uses SeqScan and nested-loop joins for SELECT.
    static std::unique_ptr<PlanNode> Plan(const BoundStatement& statement);
    // Selects indexed access paths and hashable equijoins without reading data.
    static std::unique_ptr<PlanNode> Plan(const BoundStatement& statement,
                                          const Catalog& catalog);
};

}  // namespace udb::sql
