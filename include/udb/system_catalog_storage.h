#pragma once

#include "udb/buffer_pool_manager.h"

#include <vector>

namespace udb {

// Versioned page-chain storage for the serialized system catalog. The root page
// ID is the only catalog location that must be kept in bootstrap metadata.
class SystemCatalogStorage {
public:
    static page_id_t Create(BufferPoolManager& pool);

    SystemCatalogStorage(BufferPoolManager& pool, page_id_t root_page_id);
    std::vector<unsigned char> Read() const;
    void Write(const std::vector<unsigned char>& bytes);
    page_id_t GetRootPageId() const { return root_page_id_; }

private:
    BufferPoolManager& pool_;
    page_id_t root_page_id_;
};

}  // namespace udb
