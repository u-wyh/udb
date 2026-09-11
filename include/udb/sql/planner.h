#pragma once

#include "udb/sql/bound_statement.h"
#include "udb/sql/plan.h"
#include "udb/catalog.h"

namespace udb::sql {

class Planner {
public:
    // Catalog-free planning preserves the original bound-to-plan conversion and
    // always produces SeqScan for SELECT.
    static std::unique_ptr<PlanNode> Plan(const BoundStatement& statement);
    // Uses catalog metadata only to select an indexed equality access path.
    static std::unique_ptr<PlanNode> Plan(const BoundStatement& statement,
                                          const Catalog& catalog);
};

}  // namespace udb::sql
