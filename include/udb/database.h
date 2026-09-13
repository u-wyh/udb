#pragma once

#include "udb/catalog.h"
#include "udb/log_manager.h"

#include <map>

namespace udb {

// Single owner per database; no concurrent opens/writers. Path must end in .udb;
// metadata and WAL live beside it with .meta and .wal extensions.
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
    LogManager& GetLogManager();
    const LogManager& GetLogManager() const;
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
    std::filesystem::path wal_path_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> pool_;
    std::unique_ptr<Catalog> catalog_;
    std::map<page_id_t, bool> recovery_page_states_;
};

}  // namespace udb
