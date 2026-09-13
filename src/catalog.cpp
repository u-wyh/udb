#include "udb/catalog.h"

#include "udb/tuple.h"

#include <limits>
#include <algorithm>

namespace udb {
namespace {

bool ValueLess(const Value& left, const Value& right) {
    switch (left.GetType()) {
        case TypeId::BOOLEAN: return !left.GetBoolean() && right.GetBoolean();
        case TypeId::INTEGER: return left.GetInteger() < right.GetInteger();
        case TypeId::BIGINT: return left.GetBigInt() < right.GetBigInt();
        case TypeId::VARCHAR: return left.GetVarchar() < right.GetVarchar();
        case TypeId::DOUBLE: return left.GetDouble() < right.GetDouble();
    }
    throw std::invalid_argument("Unknown statistics type");
}

}  // namespace

void Catalog::RestoreTable(const TableMetadata& metadata) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (tables_.count(metadata.GetTableId()) != 0) {
        throw std::runtime_error("Duplicate metadata table ID");
    }
    for (const auto& item : tables_) {
        if (item.second->metadata.GetTableName() == metadata.GetTableName() ||
            item.second->metadata.GetFirstPageId() == metadata.GetFirstPageId()) {
            throw std::runtime_error("Duplicate metadata table name or first page");
        }
    }
    auto entry = std::make_unique<Entry>(pool_, metadata);
    tables_.emplace(metadata.GetTableId(), std::move(entry));
}

void Catalog::RestoreNextId(table_id_t next_id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!tables_.empty() && next_id <= tables_.rbegin()->first) {
        throw std::runtime_error("Invalid metadata next table ID");
    }
    next_id_ = next_id;
}

void Catalog::RestoreIndex(const IndexMetadata& metadata) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (indexes_.count(metadata.GetIndexId()) != 0) {
        throw std::runtime_error("Duplicate metadata index ID");
    }
    const auto table = tables_.find(metadata.GetTableId());
    if (table == tables_.end()) { throw std::runtime_error("Metadata index references missing table"); }
    const auto& schema = table->second->metadata.GetSchema();
    const auto key_width = IndexKeyWidth(schema, metadata.GetColumnIndexes());
    for (const auto& item : indexes_) {
        const auto& existing = item.second->GetMetadata();
        if (existing.GetIndexName() == metadata.GetIndexName() ||
            (existing.GetTableId() == metadata.GetTableId() &&
             existing.GetColumnIndexes() == metadata.GetColumnIndexes()) ||
            existing.GetHeaderPageId() == metadata.GetHeaderPageId()) {
            throw std::runtime_error("Duplicate metadata index identity");
        }
    }
    auto tree = BPlusTree::OpenWithHeader(pool_, metadata.GetHeaderPageId());
    if (tree->GetStringMaxLength() != key_width) {
        throw std::runtime_error("Index key format does not match column");
    }
    indexes_.emplace(metadata.GetIndexId(),
                     std::unique_ptr<Index>(new Index(metadata, std::move(tree))));
}

void Catalog::RestoreNextIndexId(index_id_t next_id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!indexes_.empty() && next_id <= indexes_.rbegin()->first) {
        throw std::runtime_error("Invalid metadata next index ID");
    }
    next_index_id_ = next_id;
}

const TableMetadata& Catalog::CreateTable(const std::string& name, const Schema& schema) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty()) {
        throw std::invalid_argument("Table name must not be empty");
    }
    for (const auto& item : tables_) {
        if (item.second->metadata.GetTableName() == name) {
            throw std::invalid_argument("Table name already exists");
        }
    }
    if (next_id_ == std::numeric_limits<table_id_t>::max()) {
        throw std::overflow_error("Catalog table ID limit reached");
    }
    auto entry = std::make_unique<Entry>(pool_, next_id_, name, schema);
    const auto inserted = tables_.emplace(next_id_, std::move(entry));
    ++next_id_;
    return inserted.first->second->metadata;
}

void Catalog::DropTable(table_id_t id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto found = tables_.find(id);
    if (found == tables_.end()) { throw std::out_of_range("Table ID not found"); }
    found->second->heap.DeletePages();
    for (auto index = indexes_.begin(); index != indexes_.end();) {
        if (index->second->GetMetadata().GetTableId() != id) {
            ++index;
            continue;
        }
        index->second->GetTree().DeletePages();
        index = indexes_.erase(index);
    }
    tables_.erase(found);
}

void Catalog::DropIndex(index_id_t id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto found = indexes_.find(id);
    if (found == indexes_.end()) { throw std::out_of_range("Index ID not found"); }
    found->second->GetTree().DeletePages();
    indexes_.erase(found);
}

std::size_t Catalog::Vacuum() {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto watermark = TransactionManager::GetWatermark();
    std::size_t removed = 0;
    for (auto& [table_id, entry] : tables_) {
        static_cast<void>(table_id);
        std::vector<RID> rids;
        for (auto rid = entry->heap.GetFirstRID(); rid; rid = entry->heap.GetNextRID(*rid)) {
            rids.push_back(*rid);
        }
        for (const auto rid : rids) {
            const auto meta = entry->heap.GetTupleMeta(rid);
            if (transaction_manager_.VacuumVersion(rid, meta, watermark)) {
                entry->heap.DeleteRecord(rid);
                ++removed;
            }
        }
    }
    return removed;
}

void Catalog::DropIndex(const std::string& name) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    DropIndex(GetIndex(name).GetMetadata().GetIndexId());
}

const TableMetadata& Catalog::GetTable(table_id_t id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return tables_.at(id)->metadata;
}

const TableMetadata& Catalog::GetTable(const std::string& name) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& item : tables_) {
        if (item.second->metadata.GetTableName() == name) {
            return item.second->metadata;
        }
    }
    throw std::out_of_range("Table name not found");
}

TableHeap& Catalog::GetTableHeap(table_id_t id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return tables_.at(id)->heap;
}
TableHeap& Catalog::GetTableHeap(const std::string& name) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return GetTableHeap(GetTable(name).GetTableId());
}
const TableHeap& Catalog::GetTableHeap(table_id_t id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return tables_.at(id)->heap;
}
const TableHeap& Catalog::GetTableHeap(const std::string& name) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return GetTableHeap(GetTable(name).GetTableId());
}

std::vector<table_id_t> Catalog::ListTables() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<table_id_t> ids;
    ids.reserve(tables_.size());
    for (const auto& item : tables_) {
        ids.push_back(item.first);
    }
    return ids;
}

const TableStatistics& Catalog::AnalyzeTable(table_id_t id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& entry = *tables_.at(id);
    const auto& schema = entry.metadata.GetSchema();
    TableStatistics statistics;
    statistics.columns.resize(schema.GetColumnCount());
    std::vector<std::vector<Value>> distinct(schema.GetColumnCount());
    for (auto rid = entry.heap.GetFirstRID(); rid; rid = entry.heap.GetNextRID(*rid)) {
        if (entry.heap.GetTupleMeta(*rid).is_deleted) { continue; }
        const auto tuple = Tuple::Deserialize(entry.heap.GetRecord(*rid), schema);
        ++statistics.row_count;
        for (std::size_t column = 0; column < schema.GetColumnCount(); ++column) {
            const auto& value = tuple.GetValue(column);
            auto& column_statistics = statistics.columns[column];
            if (value.IsNull()) { ++column_statistics.null_count; continue; }
            ++column_statistics.non_null_count;
            if (!column_statistics.minimum || ValueLess(value, *column_statistics.minimum)) {
                column_statistics.minimum = value;
            }
            if (!column_statistics.maximum || ValueLess(*column_statistics.maximum, value)) {
                column_statistics.maximum = value;
            }
            auto& values = distinct[column];
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        }
    }
    for (std::size_t column = 0; column < statistics.columns.size(); ++column) {
        statistics.columns[column].distinct_count = distinct[column].size();
    }
    entry.statistics = std::move(statistics);
    return *entry.statistics;
}

const TableStatistics& Catalog::AnalyzeTable(const std::string& name) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return AnalyzeTable(GetTable(name).GetTableId());
}

bool Catalog::HasTableStatistics(table_id_t id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return tables_.at(id)->statistics.has_value();
}

const TableStatistics& Catalog::GetTableStatistics(table_id_t id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto& statistics = tables_.at(id)->statistics;
    if (!statistics) { throw std::logic_error("Table has not been analyzed"); }
    return *statistics;
}

const TableStatistics& Catalog::GetTableStatistics(const std::string& name) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return GetTableStatistics(GetTable(name).GetTableId());
}

const Index& Catalog::CreateIndex(const std::string& name, table_id_t table_id,
                                  std::size_t column_index, BPlusTreeOptions options) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return CreateIndex(name, table_id, std::vector<std::size_t>{column_index}, options);
}

const Index& Catalog::CreateIndex(const std::string& name, table_id_t table_id,
                                  const std::vector<std::size_t>& columns, BPlusTreeOptions options) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty()) { throw std::invalid_argument("Index name must not be empty"); }
    const auto table = tables_.find(table_id);
    if (table == tables_.end()) { throw std::out_of_range("Index table ID not found"); }
    const auto& schema = table->second->metadata.GetSchema();
    for (const auto column : columns) {
        if (column >= schema.GetColumnCount()) { throw std::out_of_range("Index column is out of range"); }
    }
    options.string_max_length = IndexKeyWidth(schema, columns);
    for (const auto& item : indexes_) {
        const auto& metadata = item.second->GetMetadata();
        if (metadata.GetIndexName() == name) { throw std::invalid_argument("Index name already exists"); }
        if (metadata.GetTableId() == table_id && metadata.GetColumnIndexes() == columns) {
            throw std::invalid_argument("Table column already has an index");
        }
    }
    if (next_index_id_ == std::numeric_limits<index_id_t>::max()) {
        throw std::overflow_error("Catalog index ID limit reached");
    }

    // Build first and publish last. A failed build can leave unreachable tree
    // pages, but never exposes partial IndexMetadata.
    auto tree = BPlusTree::CreateWithHeader(pool_, options);
    auto& heap = table->second->heap;
    for (auto rid = heap.GetFirstRID(); rid; rid = heap.GetNextRID(*rid)) {
        if (heap.GetTupleMeta(*rid).is_deleted) { continue; }
        const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), schema);
        const auto key = GetTupleIndexKey(tuple, columns);
        if (!key) { continue; }
        if (!tree->Insert(*key, *rid)) {
            throw std::invalid_argument("Cannot build unique index from duplicate values");
        }
    }
    IndexMetadata metadata(next_index_id_, name, table_id, columns,
                           tree->GetHeaderPageId());
    auto entry = std::unique_ptr<Index>(new Index(metadata, std::move(tree)));
    const auto inserted = indexes_.emplace(next_index_id_, std::move(entry));
    ++next_index_id_;
    return *inserted.first->second;
}

const Index& Catalog::GetIndex(index_id_t id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return *indexes_.at(id);
}

const Index& Catalog::GetIndex(const std::string& name) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& item : indexes_) {
        if (item.second->GetMetadata().GetIndexName() == name) { return *item.second; }
    }
    throw std::out_of_range("Index name not found");
}

Index& Catalog::GetIndex(index_id_t id) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return *indexes_.at(id);
}
Index& Catalog::GetIndex(const std::string& name) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& item : indexes_) {
        if (item.second->GetMetadata().GetIndexName() == name) { return *item.second; }
    }
    throw std::out_of_range("Index name not found");
}

std::vector<index_id_t> Catalog::ListIndexes() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<index_id_t> ids;
    ids.reserve(indexes_.size());
    for (const auto& item : indexes_) { ids.push_back(item.first); }
    return ids;
}

std::vector<index_id_t> Catalog::GetTableIndexes(table_id_t table_id) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (tables_.count(table_id) == 0) { throw std::out_of_range("Table ID not found"); }
    std::vector<index_id_t> ids;
    for (const auto& item : indexes_) {
        if (item.second->GetMetadata().GetTableId() == table_id) { ids.push_back(item.first); }
    }
    return ids;
}

}  // namespace udb
