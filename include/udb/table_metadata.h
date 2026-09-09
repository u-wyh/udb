#pragma once

#include "udb/page.h"
#include "udb/schema.h"

namespace udb {

using table_id_t = std::uint64_t;

// Owns descriptive data only; no record storage or I/O.
class TableMetadata {
public:
    TableMetadata(table_id_t id, std::string name, Schema schema, page_id_t first_page_id)
        : id_(id), name_(std::move(name)), schema_(std::move(schema)), first_page_id_(first_page_id) {
        if (name_.empty() || first_page_id < 0) {
            throw std::invalid_argument("Invalid table name or first page ID");
        }
    }
    table_id_t GetTableId() const { return id_; }
    const std::string& GetTableName() const { return name_; }
    const Schema& GetSchema() const { return schema_; }
    page_id_t GetFirstPageId() const { return first_page_id_; }

private:
    table_id_t id_;
    std::string name_;
    Schema schema_;
    page_id_t first_page_id_;
};

}  // namespace udb
