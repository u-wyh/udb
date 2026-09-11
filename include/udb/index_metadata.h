#pragma once

#include "udb/b_plus_tree.h"
#include "udb/table_metadata.h"

#include <memory>
#include <string>

namespace udb {

using index_id_t = std::uint64_t;

// Descriptive index data only. header_page_id remains stable across root splits.
class IndexMetadata {
public:
    IndexMetadata(index_id_t id, std::string name, table_id_t table_id,
                  std::size_t column_index, page_id_t header_page_id)
        : id_(id), name_(std::move(name)), table_id_(table_id),
          column_index_(column_index), header_page_id_(header_page_id) {
        if (name_.empty() || header_page_id < 0) {
            throw std::invalid_argument("Invalid index name or header page ID");
        }
    }

    index_id_t GetIndexId() const { return id_; }
    const std::string& GetIndexName() const { return name_; }
    table_id_t GetTableId() const { return table_id_; }
    std::size_t GetColumnIndex() const { return column_index_; }
    page_id_t GetHeaderPageId() const { return header_page_id_; }

private:
    index_id_t id_;
    std::string name_;
    table_id_t table_id_;
    std::size_t column_index_;
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
