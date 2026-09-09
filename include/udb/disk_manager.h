#pragma once

#include "udb/page.h"

#include <filesystem>
#include <fstream>

namespace udb {

// Single-owner, synchronous page I/O. Not thread-safe; do not open the same
// file through multiple managers concurrently. Flush is not an fsync guarantee.
class DiskManager {
public:
    explicit DiskManager(const std::filesystem::path& path);

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Extends the file by one zero-filled page and returns its ID.
    page_id_t AllocatePage();
    // Only allocated IDs are valid. Invalid IDs throw std::out_of_range;
    // malformed files and I/O failures throw std::runtime_error.
    void WritePage(page_id_t page_id, const Page& page);
    Page ReadPage(page_id_t page_id);

private:
    std::streamoff Offset(page_id_t page_id) const;

    std::fstream file_;
    page_id_t page_count_ = 0;
};

}  // namespace udb
