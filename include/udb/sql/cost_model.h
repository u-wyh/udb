#pragma once

#include "udb/catalog.h"
#include "udb/sql/plan.h"

namespace udb::sql {

struct CostEstimate {
    double startup_cost = 0;
    double total_cost = 0;
    double estimated_rows = 0;
};

// Coarse, deterministic units for comparing current access paths. Missing
// statistics use conservative defaults; this model performs no plan selection.
class CostModel {
public:
    static CostEstimate Estimate(const PlanNode& plan, const Catalog& catalog);
};

}  // namespace udb::sql
