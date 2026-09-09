#pragma once

#include "udb/catalog.h"

namespace udb {

// Single owner per database; no concurrent opens/writers. Path must end in .udb;
// metadata lives beside it with extension .meta. No reserved data pages.
class Database {
public:
    static std::unique_ptr<Database> Create(const std::filesystem::path& data_path, std::size_t capacity = 16);
    static std::unique_ptr<Database> Open(const std::filesystem::path& data_path, std::size_t capacity = 16);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    // No destructor I/O: explicitly Flush/Close to persist and observe failures.
    ~Database() = default;

    Catalog& GetCatalog();
    const Catalog& GetCatalog() const;
    void Flush();  // Dirty data first, then temp-file metadata replacement; no fsync.
    void Close();  // Idempotent. On failure stays open so caller can retry.
    // References to catalog/tables are invalid after Close/destruction.

private:
    Database(const std::filesystem::path& data_path, std::size_t capacity);
    void LoadMetadata();
    void SaveMetadata() const;
    void RequireOpen() const;

    std::filesystem::path data_path_;
    std::filesystem::path metadata_path_;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> pool_;
    std::unique_ptr<Catalog> catalog_;
};

}  // namespace udb
