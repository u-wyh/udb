#pragma once

#include "udb/table_heap.h"
#include "udb/table_metadata.h"

#include <map>
#include <memory>

namespace udb {

// Single-threaded, memory-only. Pool must outlive the catalog. Returned
// references stay valid until their table is dropped or catalog destruction;
// no implicit flush or reload.
class Catalog {
public:
    explicit Catalog(BufferPoolManager& pool) : pool_(pool) {}
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    // Names are case-sensitive. Empty/duplicate names throw invalid_argument.
    // Failed creation publishes no metadata and does not consume a table ID.
    // As with TableHeap, a late allocation/I/O failure can leave unreclaimed pages.
    const TableMetadata& CreateTable(const std::string& name, const Schema& schema);
    // Explicitly releases the table's complete page chain before removing its
    // metadata. Pinned pages fail without changing catalog ownership.
    // Table IDs remain monotonic. Missing IDs throw out_of_range.
    void DropTable(table_id_t id);
    const TableMetadata& GetTable(table_id_t id) const;
    const TableMetadata& GetTable(const std::string& name) const;
    TableHeap& GetTableHeap(table_id_t id);
    TableHeap& GetTableHeap(const std::string& name);
    const TableHeap& GetTableHeap(table_id_t id) const;
    const TableHeap& GetTableHeap(const std::string& name) const;
    std::vector<table_id_t> ListTables() const;  // Ascending ID order; snapshot.

private:
    friend class Database;
    void RestoreTable(const TableMetadata& metadata);
    void RestoreNextId(table_id_t next_id);
    struct Entry {
        Entry(BufferPoolManager& pool, table_id_t id, const std::string& name, const Schema& schema)
            : heap(pool), metadata(id, name, schema, heap.GetFirstPageId()) {}
        Entry(BufferPoolManager& pool, const TableMetadata& saved)
            : heap(pool, saved.GetFirstPageId()), metadata(saved) {}
        TableHeap heap;
        TableMetadata metadata;
    };

    BufferPoolManager& pool_;
    table_id_t next_id_ = 0;
    // One authoritative map avoids partially updated name/ID indexes.
    // Name lookup is a simple linear scan for this first catalog.
    std::map<table_id_t, std::unique_ptr<Entry>> tables_;
};

}  // namespace udb
