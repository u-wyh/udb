#pragma once

#include "udb/sql/bound_statement.h"
#include "udb/sql/plan.h"

namespace udb::sql {

class Planner {
public:
    // Precondition: statement was validated by Binder. Copies bound information
    // into an independent plan; no Catalog, TableHeap, or name lookup required.
    static std::unique_ptr<PlanNode> Plan(const BoundStatement& statement);
};

}  // namespace udb::sql
