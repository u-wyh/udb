#include "udb/catalog.h"

#include "udb/tuple.h"

#include <limits>

namespace udb {

void Catalog::RestoreTable(const TableMetadata& metadata) {
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
    if (!tables_.empty() && next_id <= tables_.rbegin()->first) {
        throw std::runtime_error("Invalid metadata next table ID");
    }
    next_id_ = next_id;
}

void Catalog::RestoreIndex(const IndexMetadata& metadata) {
    if (indexes_.count(metadata.GetIndexId()) != 0) {
        throw std::runtime_error("Duplicate metadata index ID");
    }
    const auto table = tables_.find(metadata.GetTableId());
    if (table == tables_.end()) { throw std::runtime_error("Metadata index references missing table"); }
    const auto& schema = table->second->metadata.GetSchema();
    if (metadata.GetColumnIndex() >= schema.GetColumnCount()) {
        throw std::runtime_error("Metadata index column is out of range");
    }
    const auto type = schema.GetColumn(metadata.GetColumnIndex()).GetType();
    if (type != TypeId::INTEGER && type != TypeId::BIGINT) {
        throw std::runtime_error("Metadata index column type is unsupported");
    }
    for (const auto& item : indexes_) {
        const auto& existing = item.second->GetMetadata();
        if (existing.GetIndexName() == metadata.GetIndexName() ||
            (existing.GetTableId() == metadata.GetTableId() &&
             existing.GetColumnIndex() == metadata.GetColumnIndex()) ||
            existing.GetHeaderPageId() == metadata.GetHeaderPageId()) {
            throw std::runtime_error("Duplicate metadata index identity");
        }
    }
    auto tree = BPlusTree::OpenWithHeader(pool_, metadata.GetHeaderPageId());
    indexes_.emplace(metadata.GetIndexId(),
                     std::unique_ptr<Index>(new Index(metadata, std::move(tree))));
}

void Catalog::RestoreNextIndexId(index_id_t next_id) {
    if (!indexes_.empty() && next_id <= indexes_.rbegin()->first) {
        throw std::runtime_error("Invalid metadata next index ID");
    }
    next_index_id_ = next_id;
}

const TableMetadata& Catalog::CreateTable(const std::string& name, const Schema& schema) {
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
    const auto found = indexes_.find(id);
    if (found == indexes_.end()) { throw std::out_of_range("Index ID not found"); }
    found->second->GetTree().DeletePages();
    indexes_.erase(found);
}

void Catalog::DropIndex(const std::string& name) {
    DropIndex(GetIndex(name).GetMetadata().GetIndexId());
}

const TableMetadata& Catalog::GetTable(table_id_t id) const { return tables_.at(id)->metadata; }

const TableMetadata& Catalog::GetTable(const std::string& name) const {
    for (const auto& item : tables_) {
        if (item.second->metadata.GetTableName() == name) {
            return item.second->metadata;
        }
    }
    throw std::out_of_range("Table name not found");
}

TableHeap& Catalog::GetTableHeap(table_id_t id) { return tables_.at(id)->heap; }
TableHeap& Catalog::GetTableHeap(const std::string& name) { return GetTableHeap(GetTable(name).GetTableId()); }
const TableHeap& Catalog::GetTableHeap(table_id_t id) const { return tables_.at(id)->heap; }
const TableHeap& Catalog::GetTableHeap(const std::string& name) const { return GetTableHeap(GetTable(name).GetTableId()); }

std::vector<table_id_t> Catalog::ListTables() const {
    std::vector<table_id_t> ids;
    ids.reserve(tables_.size());
    for (const auto& item : tables_) {
        ids.push_back(item.first);
    }
    return ids;
}

const Index& Catalog::CreateIndex(const std::string& name, table_id_t table_id,
                                  std::size_t column_index, BPlusTreeOptions options) {
    if (name.empty()) { throw std::invalid_argument("Index name must not be empty"); }
    const auto table = tables_.find(table_id);
    if (table == tables_.end()) { throw std::out_of_range("Index table ID not found"); }
    const auto& schema = table->second->metadata.GetSchema();
    if (column_index >= schema.GetColumnCount()) {
        throw std::out_of_range("Index column is out of range");
    }
    const auto type = schema.GetColumn(column_index).GetType();
    if (type != TypeId::INTEGER && type != TypeId::BIGINT) {
        throw std::invalid_argument("Index column must be INTEGER or BIGINT");
    }
    for (const auto& item : indexes_) {
        const auto& metadata = item.second->GetMetadata();
        if (metadata.GetIndexName() == name) { throw std::invalid_argument("Index name already exists"); }
        if (metadata.GetTableId() == table_id && metadata.GetColumnIndex() == column_index) {
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
        const auto tuple = Tuple::Deserialize(heap.GetRecord(*rid), schema);
        const auto& value = tuple.GetValue(column_index);
        if (value.IsNull()) { continue; }
        const auto key = type == TypeId::INTEGER
            ? static_cast<std::int64_t>(value.GetInteger()) : value.GetBigInt();
        if (!tree->Insert(key, *rid)) {
            throw std::invalid_argument("Cannot build unique index from duplicate values");
        }
    }
    IndexMetadata metadata(next_index_id_, name, table_id, column_index,
                           tree->GetHeaderPageId());
    auto entry = std::unique_ptr<Index>(new Index(metadata, std::move(tree)));
    const auto inserted = indexes_.emplace(next_index_id_, std::move(entry));
    ++next_index_id_;
    return *inserted.first->second;
}

const Index& Catalog::GetIndex(index_id_t id) const { return *indexes_.at(id); }

const Index& Catalog::GetIndex(const std::string& name) const {
    for (const auto& item : indexes_) {
        if (item.second->GetMetadata().GetIndexName() == name) { return *item.second; }
    }
    throw std::out_of_range("Index name not found");
}

Index& Catalog::GetIndex(index_id_t id) { return *indexes_.at(id); }
Index& Catalog::GetIndex(const std::string& name) {
    for (auto& item : indexes_) {
        if (item.second->GetMetadata().GetIndexName() == name) { return *item.second; }
    }
    throw std::out_of_range("Index name not found");
}

std::vector<index_id_t> Catalog::ListIndexes() const {
    std::vector<index_id_t> ids;
    ids.reserve(indexes_.size());
    for (const auto& item : indexes_) { ids.push_back(item.first); }
    return ids;
}

std::vector<index_id_t> Catalog::GetTableIndexes(table_id_t table_id) const {
    if (tables_.count(table_id) == 0) { throw std::out_of_range("Table ID not found"); }
    std::vector<index_id_t> ids;
    for (const auto& item : indexes_) {
        if (item.second->GetMetadata().GetTableId() == table_id) { ids.push_back(item.first); }
    }
    return ids;
}

}  // namespace udb
