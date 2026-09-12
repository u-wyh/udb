#pragma once

#include "udb/b_plus_tree.h"
#include "udb/table_metadata.h"
#include "udb/value.h"
#include "udb/tuple.h"

#include <memory>
#include <string>
#include <set>

namespace udb {

using index_id_t = std::uint64_t;

inline std::optional<IndexKey> GetIndexKey(const Value& value) {
    if (value.IsNull()) { return std::nullopt; }
    if (value.GetType() == TypeId::INTEGER) { return IndexKey(value.GetInteger()); }
    if (value.GetType() == TypeId::BIGINT) { return IndexKey(value.GetBigInt()); }
    if (value.GetType() == TypeId::VARCHAR) { return IndexKey(value.GetVarchar()); }
    throw std::invalid_argument("Unsupported index key type");
}

inline std::size_t IndexKeyWidth(const Schema& schema, const std::vector<std::size_t>& columns) {
    if (columns.empty()) { throw std::invalid_argument("Index needs at least one column"); }
    std::set<std::size_t> seen;
    std::size_t length = 0;
    for (const auto i : columns) {
        if (i >= schema.GetColumnCount() || !seen.insert(i).second) {
            throw std::invalid_argument("Invalid or duplicate index column");
        }
        const auto& column = schema.GetColumn(i);
        if (column.GetType() == TypeId::VARCHAR) {
            length += columns.size() == 1 ? column.GetMaxLength() : 2ULL * column.GetMaxLength() + 2;
        } else if (column.GetType() == TypeId::INTEGER || column.GetType() == TypeId::BIGINT) {
            if (columns.size() > 1) { length += 8; }
        } else {
            throw std::invalid_argument("Unsupported index column type");
        }
        if (length > 1024) { throw std::invalid_argument("Encoded index key exceeds 1024 bytes"); }
    }
    return length;
}

inline std::optional<IndexKey> GetTupleIndexKey(const Tuple& tuple, const std::vector<std::size_t>& columns) {
    std::vector<IndexKey> values;
    for (const auto column : columns) {
        const auto value = GetIndexKey(tuple.GetValue(column));
        if (!value) { return std::nullopt; }
        values.push_back(*value);
    }
    if (values.empty()) { throw std::invalid_argument("Index has no columns"); }
    return values.size() == 1 ? values.front() : MakeCompositeKey(values);
}

// Descriptive index data only. header_page_id remains stable across root splits.
class IndexMetadata {
public:
    IndexMetadata(index_id_t id, std::string name, table_id_t table_id,
                  std::size_t column_index, page_id_t header_page_id)
        : IndexMetadata(id, std::move(name), table_id, std::vector<std::size_t>{column_index}, header_page_id) {}
    IndexMetadata(index_id_t id, std::string name, table_id_t table_id,
                  std::vector<std::size_t> columns, page_id_t header_page_id)
        : id_(id), name_(std::move(name)), table_id_(table_id),
          column_indexes_(std::move(columns)), header_page_id_(header_page_id) {
        if (name_.empty() || header_page_id < 0 || column_indexes_.empty()) {
            throw std::invalid_argument("Invalid index name or header page ID");
        }
    }

    index_id_t GetIndexId() const { return id_; }
    const std::string& GetIndexName() const { return name_; }
    table_id_t GetTableId() const { return table_id_; }
    std::size_t GetColumnIndex() const { return column_indexes_.front(); }
    const std::vector<std::size_t>& GetColumnIndexes() const { return column_indexes_; }
    page_id_t GetHeaderPageId() const { return header_page_id_; }

private:
    index_id_t id_;
    std::string name_;
    table_id_t table_id_;
    std::vector<std::size_t> column_indexes_;
    page_id_t header_page_id_;
};

class Catalog;

class Index {
public:
    const IndexMetadata& GetMetadata() const { return metadata_; }
    BPlusTree& GetTree() { return *tree_; }
    const BPlusTree& GetTree() const { return *tree_; }

private:
    friend class Catalog;
    Index(IndexMetadata metadata, std::unique_ptr<BPlusTree> tree)
        : metadata_(std::move(metadata)), tree_(std::move(tree)) {}

    IndexMetadata metadata_;
    std::unique_ptr<BPlusTree> tree_;
};

}  // namespace udb
