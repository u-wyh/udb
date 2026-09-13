#include "udb/database.h"

#include <fstream>
#include <limits>

namespace udb {
namespace {

// v4: magic:u64, version:u32, table_count:u32, next_table_id:u64,
//     free_page_count:u64, then that many free_page_id:u64 values.
// Table: id:u64, name:(u32 length + bytes), first_page:u64, column_count:u32.
// Column: name:(u32 length + bytes), type:u8 (0..4), max_length:u32.
// After tables: next_index_id:u64, index_count:u64, then Index entries:
// id:u64, name:(u32 length + bytes), table_id:u64, column_count:u64,
// column_indexes:u64[], header_page_id:u64. v3 stored one column without a count.
// All integers are little-endian. No struct layouts or native string objects.
constexpr std::uint64_t kMagic = 0x314154454d424455;  // "UDBMETA1"
constexpr std::uint32_t kVersion = 4;

std::filesystem::path MetadataPath(const std::filesystem::path& path) {
    if (path.empty() || path.extension() != ".udb") {
        throw std::invalid_argument("Database path must end in .udb");
    }
    auto result = path;
    return result.replace_extension(".meta");
}

std::filesystem::path WalPath(const std::filesystem::path& path) {
    static_cast<void>(MetadataPath(path));
    auto result = path;
    return result.replace_extension(".wal");
}

std::filesystem::path TemporaryPath(const std::filesystem::path& path) {
    auto result = path;
    result += ".tmp";
    return result;
}

bool Exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path) || std::filesystem::is_symlink(path);
}

void Write(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

void WriteString(std::vector<unsigned char>& bytes, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("Metadata string is too long");
    }
    Write(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::uint8_t EncodeType(TypeId type) {
    switch (type) {
        case TypeId::BOOLEAN: return 0;
        case TypeId::INTEGER: return 1;
        case TypeId::BIGINT: return 2;
        case TypeId::VARCHAR: return 3;
        case TypeId::DOUBLE: return 4;
    }
    throw std::runtime_error("Unknown metadata TypeId");
}

TypeId DecodeType(std::uint64_t type) {
    switch (type) {
        case 0: return TypeId::BOOLEAN;
        case 1: return TypeId::INTEGER;
        case 2: return TypeId::BIGINT;
        case 3: return TypeId::VARCHAR;
        case 4: return TypeId::DOUBLE;
    }
    throw std::runtime_error("Unknown metadata TypeId");
}

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) {
        const auto size = std::filesystem::file_size(path);
        if (size > std::numeric_limits<std::size_t>::max() ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::runtime_error("Metadata file is too large");
        }
        bytes_.resize(static_cast<std::size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input || (size != 0 && !input.read(bytes_.data(), static_cast<std::streamsize>(size)))) {
            throw std::runtime_error("Cannot read metadata file");
        }
    }
    std::size_t Remaining() const { return bytes_.size() - position_; }
    std::uint64_t Read(std::size_t width) {
        if (width > Remaining()) { throw std::runtime_error("Truncated metadata"); }
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < width; ++i) {
            result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[position_++])) << (8 * i);
        }
        return result;
    }
    std::string String() {
        const auto size = Read(4);
        if (size > Remaining()) { throw std::runtime_error("Truncated metadata string"); }
        const std::string value(bytes_.data() + position_, static_cast<std::size_t>(size));
        position_ += static_cast<std::size_t>(size);
        return value;
    }

private:
    std::vector<char> bytes_;
    std::size_t position_ = 0;
};

}  // namespace

Database::Database(const std::filesystem::path& path, std::size_t capacity)
    : data_path_(path), metadata_path_(MetadataPath(path)),
      wal_path_(WalPath(path)), log_manager_(std::make_unique<LogManager>(wal_path_)),
      disk_(std::make_unique<DiskManager>(path)),
      pool_(std::make_unique<BufferPoolManager>(*disk_, capacity, log_manager_.get())),
      catalog_(std::make_unique<Catalog>(*pool_, log_manager_.get())) {}

std::unique_ptr<Database> Database::Create(const std::filesystem::path& path, std::size_t capacity) {
    const auto metadata = MetadataPath(path);
    const auto wal = WalPath(path);
    if (capacity == 0) { throw std::invalid_argument("Buffer pool capacity must be positive"); }
    if (Exists(path) || Exists(metadata) || Exists(wal) || Exists(TemporaryPath(metadata))) {
        throw std::runtime_error("Database files already exist");
    }
    try {
        auto database = std::unique_ptr<Database>(new Database(path, capacity));
        database->Flush();
        return database;
    } catch (...) {
        // Only the new data path owned by this failed Create is removed.
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(wal, error);
        throw;
    }
}

std::unique_ptr<Database> Database::Open(const std::filesystem::path& path, std::size_t capacity) {
    const auto metadata = MetadataPath(path);
    if (capacity == 0) { throw std::invalid_argument("Buffer pool capacity must be positive"); }
    if (!std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(metadata)) {
        throw std::runtime_error("Database data or metadata file is missing");
    }
    auto database = std::unique_ptr<Database>(new Database(path, capacity));
    database->LoadMetadata();
    return database;
}

void Database::RequireOpen() const {
    if (!catalog_) { throw std::logic_error("Database is closed"); }
}

Catalog& Database::GetCatalog() { RequireOpen(); return *catalog_; }
const Catalog& Database::GetCatalog() const { RequireOpen(); return *catalog_; }
LogManager& Database::GetLogManager() { RequireOpen(); return *log_manager_; }
const LogManager& Database::GetLogManager() const { RequireOpen(); return *log_manager_; }

void Database::Flush() {
    RequireOpen();
    log_manager_->Flush();
    pool_->FlushAllPages();
    SaveMetadata();
}

void Database::Close() {
    if (!catalog_) { return; }
    Flush();
    catalog_.reset();
    pool_.reset();
    disk_.reset();
    log_manager_.reset();
}

void Database::SaveMetadata() const {
    const auto ids = catalog_->ListTables();
    if (ids.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("Too many metadata tables");
    }
    std::vector<unsigned char> bytes;
    Write(bytes, kMagic, 8);
    Write(bytes, kVersion, 4);
    Write(bytes, ids.size(), 4);
    Write(bytes, catalog_->next_id_, 8);
    Write(bytes, disk_->GetFreePageIds().size(), 8);
    for (const auto page_id : disk_->GetFreePageIds()) {
        Write(bytes, static_cast<std::uint64_t>(page_id), 8);
    }
    for (const auto id : ids) {
        const auto& table = catalog_->GetTable(id);
        Write(bytes, id, 8);
        WriteString(bytes, table.GetTableName());
        Write(bytes, static_cast<std::uint64_t>(table.GetFirstPageId()), 8);
        Write(bytes, table.GetSchema().GetColumnCount(), 4);
        for (const auto& column : table.GetSchema().GetColumns()) {
            WriteString(bytes, column.GetName());
            Write(bytes, EncodeType(column.GetType()), 1);
            Write(bytes, column.GetMaxLength(), 4);
        }
    }
    const auto index_ids = catalog_->ListIndexes();
    Write(bytes, catalog_->next_index_id_, 8);
    Write(bytes, index_ids.size(), 8);
    for (const auto id : index_ids) {
        const auto& metadata = catalog_->GetIndex(id).GetMetadata();
        Write(bytes, id, 8);
        WriteString(bytes, metadata.GetIndexName());
        Write(bytes, metadata.GetTableId(), 8);
        Write(bytes, metadata.GetColumnIndexes().size(), 8);
        for (const auto column : metadata.GetColumnIndexes()) { Write(bytes, column, 8); }
        Write(bytes, static_cast<std::uint64_t>(metadata.GetHeaderPageId()), 8);
    }
    if (bytes.size() > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Metadata file is too large");
    }
    const auto temporary = TemporaryPath(metadata_path_);
    if (Exists(temporary)) { throw std::runtime_error("Metadata temporary path already exists"); }
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) { throw std::runtime_error("Cannot write metadata temporary file"); }
        output.close();
        if (!output) { throw std::runtime_error("Cannot close metadata temporary file"); }
        std::filesystem::rename(temporary, metadata_path_);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}

void Database::LoadMetadata() {
    Reader reader(metadata_path_);
    if (reader.Read(8) != kMagic) { throw std::runtime_error("Invalid metadata magic"); }
    const auto version = reader.Read(4);
    if (version != 1 && version != 2 && version != 3 && version != kVersion) {
        throw std::runtime_error("Unsupported metadata version");
    }
    const auto count = reader.Read(4);
    const auto next_id = reader.Read(8);
    std::vector<page_id_t> free_pages;
    if (version >= 2) {
        const auto free_count = reader.Read(8);
        if (free_count > reader.Remaining() / 8) { throw std::runtime_error("Invalid metadata free page count"); }
        free_pages.reserve(static_cast<std::size_t>(free_count));
        for (std::uint64_t i = 0; i < free_count; ++i) {
            const auto page_id = reader.Read(8);
            if (page_id > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
                throw std::runtime_error("Invalid metadata free page ID");
            }
            free_pages.push_back(static_cast<page_id_t>(page_id));
        }
    }
    disk_->RestoreFreePageIds(free_pages);
    if (count > reader.Remaining() / 24) { throw std::runtime_error("Invalid metadata table count"); }
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto id = reader.Read(8);
        const auto name = reader.String();
        const auto first = reader.Read(8);
        if (first > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
            throw std::runtime_error("Invalid metadata first page ID");
        }
        const auto column_count = reader.Read(4);
        if (column_count > reader.Remaining() / 9) { throw std::runtime_error("Invalid metadata column count"); }
        std::vector<Column> columns;
        for (std::uint64_t j = 0; j < column_count; ++j) {
            auto column_name = reader.String();
            const auto type = DecodeType(reader.Read(1));
            const auto max_length = static_cast<std::uint32_t>(reader.Read(4));
            columns.emplace_back(std::move(column_name), type, max_length);
        }
        catalog_->RestoreTable(TableMetadata(id, name, Schema(std::move(columns)), static_cast<page_id_t>(first)));
    }
    index_id_t next_index_id = 0;
    if (version >= 3) {
        next_index_id = reader.Read(8);
        const auto index_count = reader.Read(8);
        if (index_count > reader.Remaining() / 36) { throw std::runtime_error("Invalid metadata index count"); }
        for (std::uint64_t i = 0; i < index_count; ++i) {
            const auto id = reader.Read(8);
            const auto name = reader.String();
            const auto table_id = reader.Read(8);
            const auto column_count = version >= 4 ? reader.Read(8) : 1;
            if (column_count == 0 || column_count > reader.Remaining() / 8) {
                throw std::runtime_error("Invalid metadata index column count");
            }
            std::vector<std::size_t> columns;
            for (std::uint64_t column = 0; column < column_count; ++column) {
                const auto value = reader.Read(8);
                if (value > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("Invalid metadata column index");
                }
                columns.push_back(static_cast<std::size_t>(value));
            }
            const auto header_page_id = reader.Read(8);
            if (header_page_id > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
                throw std::runtime_error("Invalid metadata index field");
            }
            catalog_->RestoreIndex(IndexMetadata(id, name, table_id,
                std::move(columns), static_cast<page_id_t>(header_page_id)));
        }
    }
    if (reader.Remaining() != 0) { throw std::runtime_error("Trailing bytes in metadata"); }
    catalog_->RestoreNextId(next_id);
    catalog_->RestoreNextIndexId(next_index_id);
}

}  // namespace udb
