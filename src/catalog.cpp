#include "udb/catalog.h"

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

}  // namespace udb
