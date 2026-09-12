#include "udb/sql/cost_model.h"

#include <algorithm>
#include <cmath>

namespace udb::sql {
namespace {

constexpr double kDefaultRows = 1000.0;
constexpr double kDefaultEqualitySelectivity = 0.1;
constexpr double kRangeSelectivity = 1.0 / 3.0;
constexpr double kHeapLookupCost = 2.0;
constexpr double kIndexOnlyRowCost = 0.2;

double TableRows(const Catalog& catalog, table_id_t table_id) {
    return catalog.HasTableStatistics(table_id)
        ? static_cast<double>(catalog.GetTableStatistics(table_id).row_count) : kDefaultRows;
}

double NonNullRows(const Catalog& catalog, table_id_t table_id,
                   std::optional<std::size_t> column) {
    if (!column || !catalog.HasTableStatistics(table_id)) { return TableRows(catalog, table_id); }
    return static_cast<double>(catalog.GetTableStatistics(table_id).columns.at(*column).non_null_count);
}

double EqualityRows(const Catalog& catalog, const Index& index) {
    const auto table_id = index.GetMetadata().GetTableId();
    const auto table_rows = TableRows(catalog, table_id);
    if (index.GetTree().IsUnique()) { return std::min(1.0, table_rows); }
    const auto& columns = index.GetMetadata().GetColumnIndexes();
    if (columns.size() == 1 && catalog.HasTableStatistics(table_id)) {
        const auto& column = catalog.GetTableStatistics(table_id).columns.at(columns[0]);
        if (column.distinct_count != 0) {
            return static_cast<double>(column.non_null_count) /
                   static_cast<double>(column.distinct_count);
        }
        return 0;
    }
    return table_rows * kDefaultEqualitySelectivity;
}

double ApplyLimit(double rows, const std::optional<std::size_t>& limit) {
    return limit ? std::min(rows, static_cast<double>(*limit)) : rows;
}

double RowsRead(double rows, const std::optional<std::size_t>& limit, std::size_t offset) {
    if (!limit) { return rows; }
    return std::min(rows, static_cast<double>(offset) + static_cast<double>(*limit));
}

CostEstimate IndexCost(double table_rows, double matches, bool index_only,
                       const std::optional<std::size_t>& limit, std::size_t offset) {
    const auto startup = std::log2(std::max(2.0, table_rows));
    const auto read = RowsRead(matches, limit, offset);
    return {startup, startup + read * (index_only ? kIndexOnlyRowCost : kHeapLookupCost),
            ApplyLimit(std::max(0.0, matches - static_cast<double>(offset)), limit)};
}

}  // namespace

CostEstimate CostModel::Estimate(const PlanNode& plan, const Catalog& catalog) {
    switch (plan.GetType()) {
        case PlanType::SeqScan: {
            const auto& scan = dynamic_cast<const SeqScanPlan&>(plan);
            const auto rows = TableRows(catalog, scan.GetTableId());
            const auto matches = scan.GetPredicate() ? rows * 0.25 : rows;
            return {0, rows, ApplyLimit(std::max(0.0, matches - scan.GetOffset()), scan.GetLimit())};
        }
        case PlanType::IndexScan: {
            const auto& scan = dynamic_cast<const IndexScanPlan&>(plan);
            const auto& index = catalog.GetIndex(scan.GetIndexId());
            return IndexCost(TableRows(catalog, scan.GetTableId()), EqualityRows(catalog, index),
                             false, scan.GetLimit(), scan.GetOffset());
        }
        case PlanType::IndexRangeScan: {
            const auto& scan = dynamic_cast<const IndexRangeScanPlan&>(plan);
            const auto& index = catalog.GetIndex(scan.GetIndexId());
            const auto& columns = index.GetMetadata().GetColumnIndexes();
            const auto column = columns.size() == 1
                ? std::optional<std::size_t>(columns[0]) : std::nullopt;
            const auto rows = TableRows(catalog, scan.GetTableId());
            const auto matches = NonNullRows(catalog, scan.GetTableId(), column) * kRangeSelectivity;
            return IndexCost(rows, matches, false, scan.GetLimit(), scan.GetOffset());
        }
        case PlanType::IndexOnlyScan: {
            const auto& scan = dynamic_cast<const IndexOnlyScanPlan&>(plan);
            const auto& index = catalog.GetIndex(scan.GetIndexId());
            const auto rows = TableRows(catalog, scan.GetTableId());
            const auto matches = scan.GetExactKey() ? EqualityRows(catalog, index) :
                NonNullRows(catalog, scan.GetTableId(), scan.GetColumnIndex()) * kRangeSelectivity;
            return IndexCost(rows, matches, true, scan.GetLimit(), scan.GetOffset());
        }
        default:
            throw std::invalid_argument("Cost model supports scan plans only");
    }
}

}  // namespace udb::sql
