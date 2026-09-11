#include "udb/sql/executor.h"

namespace udb::sql {
namespace {

bool SameColumn(const Column& a, const Column& b) {
    return a.GetName() == b.GetName() && a.GetType() == b.GetType() && a.GetMaxLength() == b.GetMaxLength();
}

void CheckSchema(const Schema& planned, const Schema& actual) {
    if (planned.GetColumnCount() != actual.GetColumnCount()) {
        throw std::invalid_argument("Plan target schema does not match catalog");
    }
    for (std::size_t i = 0; i < planned.GetColumnCount(); ++i) {
        if (!SameColumn(planned.GetColumn(i), actual.GetColumn(i))) {
            throw std::invalid_argument("Plan target schema does not match catalog");
        }
    }
}

}  // namespace

ExecutionResult Executor::Execute(const PlanNode& plan) {
    switch (plan.GetType()) {
        case PlanType::CreateTable: {
            const auto& create = dynamic_cast<const CreateTablePlan&>(plan);
            ExecutionResult result{PlanType::CreateTable};
            catalog_.CreateTable(create.GetTableName(), create.GetTableSchema());
            return result;
        }
        case PlanType::Insert: {
            const auto& insert = dynamic_cast<const InsertPlan&>(plan);
            const auto& schema = catalog_.GetTable(insert.GetTableId()).GetSchema();
            CheckSchema(insert.GetTableSchema(), schema);
            const Tuple tuple(schema, insert.GetValues());
            const auto record = tuple.Serialize(schema);
            ExecutionResult result{PlanType::Insert};
            result.inserted_rid = catalog_.GetTableHeap(insert.GetTableId()).InsertRecord(record);
            result.affected_rows = 1;
            return result;
        }
        case PlanType::SeqScan: {
            const auto& scan = dynamic_cast<const SeqScanPlan&>(plan);
            const auto& source = catalog_.GetTable(scan.GetTableId()).GetSchema();
            const auto& output = scan.GetOutputSchema();
            const auto& indexes = scan.GetColumnIndexes();
            if (indexes.size() != output.GetColumnCount()) {
                throw std::invalid_argument("Plan projection size mismatch");
            }
            for (std::size_t i = 0; i < indexes.size(); ++i) {
                if (indexes[i] >= source.GetColumnCount() || !SameColumn(source.GetColumn(indexes[i]), output.GetColumn(i))) {
                    throw std::invalid_argument("Plan projection does not match catalog");
                }
            }
            ExecutionResult result{PlanType::SeqScan};
            result.output_schema = output;
            const auto& heap = catalog_.GetTableHeap(scan.GetTableId());
            for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
                const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), source);
                std::vector<Value> values;
                values.reserve(indexes.size());
                for (const auto index : indexes) { values.push_back(tuple.GetValue(index)); }
                result.rows.emplace_back(output, std::move(values));
            }
            return result;
        }
    }
    throw std::invalid_argument("Unsupported plan type");
}

}  // namespace udb::sql
