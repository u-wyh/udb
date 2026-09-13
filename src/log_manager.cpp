#include "udb/log_manager.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace udb {

class LogRecordDecoder {
public:
    static LogRecord Make(LogRecordType type, transaction_id_t transaction_id,
                          std::optional<page_id_t> page_id,
                          std::optional<Page> before, std::optional<Page> after,
                          lsn_t lsn) {
        LogRecord record(type, transaction_id, page_id, std::move(before), std::move(after));
        record.lsn_ = lsn;
        return record;
    }
};

namespace {

constexpr std::uint64_t kMagic = 0x31304c4157424455ULL;  // "UDBWAL01"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 48;

void Write(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

void Overwrite(std::vector<unsigned char>& bytes, std::size_t offset,
               std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    }
}

std::uint64_t Read(const std::vector<unsigned char>& bytes, std::size_t offset,
                   std::size_t width) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        throw std::runtime_error("Truncated WAL record");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

std::uint32_t Checksum(const std::vector<unsigned char>& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::size_t PayloadSize(LogRecordType type) {
    switch (type) {
        case LogRecordType::Begin:
        case LogRecordType::PageAllocate:
        case LogRecordType::Commit:
        case LogRecordType::Abort: return 0;
        case LogRecordType::PageFree: return PAGE_SIZE;
        case LogRecordType::PageWrite: return 2 * PAGE_SIZE;
    }
    throw std::runtime_error("Unknown WAL record type");
}

LogRecordType DecodeType(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(LogRecordType::Abort)) {
        throw std::runtime_error("Unknown WAL record type");
    }
    return static_cast<LogRecordType>(value);
}

std::vector<unsigned char> Encode(const LogRecord& record) {
    record.Validate();
    std::vector<unsigned char> bytes;
    bytes.reserve(kHeaderSize + PayloadSize(record.GetType()));
    Write(bytes, kMagic, 8);
    Write(bytes, kVersion, 4);
    Write(bytes, kHeaderSize + PayloadSize(record.GetType()), 4);
    Write(bytes, 0, 4);  // checksum
    Write(bytes, static_cast<std::uint8_t>(record.GetType()), 1);
    Write(bytes, 0, 3);
    Write(bytes, record.GetLsn(), 8);
    Write(bytes, record.GetTransactionId(), 8);
    Write(bytes, record.GetPageId()
                     ? static_cast<std::uint64_t>(*record.GetPageId())
                     : std::numeric_limits<std::uint64_t>::max(), 8);
    if (record.GetBeforeImage()) {
        const auto& data = record.GetBeforeImage()->data;
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    if (record.GetAfterImage()) {
        const auto& data = record.GetAfterImage()->data;
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    Overwrite(bytes, 16, Checksum(bytes), 4);
    return bytes;
}

std::vector<unsigned char> ReadFile(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("WAL file is too large");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()),
                                            static_cast<std::streamsize>(size)))) {
        throw std::runtime_error("Cannot read WAL file");
    }
    return bytes;
}

std::vector<LogRecord> Decode(const std::vector<unsigned char>& file,
                              std::size_t* valid_size) {
    std::vector<LogRecord> records;
    std::size_t position = 0;
    lsn_t expected_lsn = 0;
    while (position < file.size()) {
        if (file.size() - position < kHeaderSize) { break; }
        if (Read(file, position, 8) != kMagic || Read(file, position + 8, 4) != kVersion) {
            throw std::runtime_error("Invalid WAL record header");
        }
        const auto size = Read(file, position + 12, 4);
        const auto type = DecodeType(Read(file, position + 20, 1));
        const auto expected_size = kHeaderSize + PayloadSize(type);
        if (size != expected_size) { throw std::runtime_error("Invalid WAL record size"); }
        if (size > file.size() - position) { break; }
        std::vector<unsigned char> encoded(file.begin() + static_cast<std::ptrdiff_t>(position),
                                           file.begin() + static_cast<std::ptrdiff_t>(position + size));
        const auto stored_checksum = Read(encoded, 16, 4);
        Overwrite(encoded, 16, 0, 4);
        if (Checksum(encoded) != stored_checksum) { throw std::runtime_error("WAL checksum mismatch"); }
        const auto lsn = Read(encoded, 24, 8);
        if (lsn != expected_lsn) { throw std::runtime_error("Non-contiguous WAL LSN sequence"); }
        if (expected_lsn == std::numeric_limits<lsn_t>::max()) {
            throw std::runtime_error("WAL LSN sequence overflow");
        }
        ++expected_lsn;
        const auto transaction_id = Read(encoded, 32, 8);
        const auto encoded_page_id = Read(encoded, 40, 8);
        std::optional<page_id_t> page_id;
        if (encoded_page_id != std::numeric_limits<std::uint64_t>::max()) {
            if (encoded_page_id > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
                throw std::runtime_error("Invalid WAL page ID");
            }
            page_id = static_cast<page_id_t>(encoded_page_id);
        }
        std::optional<Page> before;
        std::optional<Page> after;
        std::size_t payload = kHeaderSize;
        if (type == LogRecordType::PageWrite || type == LogRecordType::PageFree) {
            before.emplace();
            std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(payload), PAGE_SIZE,
                        before->data.begin());
            payload += PAGE_SIZE;
        }
        if (type == LogRecordType::PageWrite) {
            after.emplace();
            std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(payload), PAGE_SIZE,
                        after->data.begin());
        }
        auto record = LogRecordDecoder::Make(type, transaction_id, page_id,
                                             std::move(before), std::move(after), lsn);
        record.Validate();
        records.push_back(std::move(record));
        position += static_cast<std::size_t>(size);
    }
    *valid_size = position;
    return records;
}

}  // namespace

LogRecord LogRecord::Begin(transaction_id_t transaction_id) {
    return LogRecord(LogRecordType::Begin, transaction_id);
}
LogRecord LogRecord::PageWrite(transaction_id_t transaction_id, page_id_t page_id,
                               const Page& before, const Page& after) {
    return LogRecord(LogRecordType::PageWrite, transaction_id, page_id, before, after);
}
LogRecord LogRecord::PageAllocate(transaction_id_t transaction_id, page_id_t page_id) {
    return LogRecord(LogRecordType::PageAllocate, transaction_id, page_id);
}
LogRecord LogRecord::PageFree(transaction_id_t transaction_id, page_id_t page_id,
                              const Page& before) {
    return LogRecord(LogRecordType::PageFree, transaction_id, page_id, before);
}
LogRecord LogRecord::Commit(transaction_id_t transaction_id) {
    return LogRecord(LogRecordType::Commit, transaction_id);
}
LogRecord LogRecord::Abort(transaction_id_t transaction_id) {
    return LogRecord(LogRecordType::Abort, transaction_id);
}

void LogRecord::Validate() const {
    const bool has_page = page_id_.has_value();
    const bool has_before = before_image_.has_value();
    const bool has_after = after_image_.has_value();
    if (has_page && *page_id_ < 0) { throw std::invalid_argument("WAL page ID must be nonnegative"); }
    switch (type_) {
        case LogRecordType::Begin:
        case LogRecordType::Commit:
        case LogRecordType::Abort:
            if (has_page || has_before || has_after) {
                throw std::invalid_argument("Transaction WAL record has page data");
            }
            return;
        case LogRecordType::PageAllocate:
            if (!has_page || has_before || has_after) {
                throw std::invalid_argument("Invalid PAGE_ALLOC WAL record");
            }
            return;
        case LogRecordType::PageFree:
            if (!has_page || !has_before || has_after) {
                throw std::invalid_argument("Invalid PAGE_FREE WAL record");
            }
            return;
        case LogRecordType::PageWrite:
            if (!has_page || !has_before || !has_after) {
                throw std::invalid_argument("Invalid PAGE_WRITE WAL record");
            }
            return;
    }
    throw std::invalid_argument("Unknown WAL record type");
}

LogManager::LogManager(const std::filesystem::path& path) : path_(path) {
    if (path.empty() || path.extension() != ".wal") {
        throw std::invalid_argument("WAL path must end in .wal");
    }
    if (!std::filesystem::exists(path)) {
        std::ofstream created(path, std::ios::binary | std::ios::app);
        created.close();
        if (!created) { throw std::runtime_error("Cannot create WAL file"); }
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("WAL path must be a regular file");
    }
    const auto file = ReadFile(path);
    std::size_t valid_size = 0;
    records_ = Decode(file, &valid_size);
    if (valid_size != file.size()) { std::filesystem::resize_file(path, valid_size); }
    next_lsn_ = static_cast<lsn_t>(records_.size());
    if (!records_.empty()) { persistent_lsn_ = records_.back().GetLsn(); }
    output_.open(path, std::ios::binary | std::ios::app);
    if (!output_) { throw std::runtime_error("Cannot open WAL file"); }
}

lsn_t LogManager::Append(LogRecord record) {
    if (next_lsn_ == std::numeric_limits<lsn_t>::max()) {
        throw std::overflow_error("WAL LSN limit reached");
    }
    record.Validate();
    record.lsn_ = next_lsn_;
    const auto encoded = Encode(record);
    records_.reserve(records_.size() + 1);
    output_.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
    if (!output_) { throw std::runtime_error("Cannot append WAL record"); }
    records_.push_back(std::move(record));
    return next_lsn_++;
}

void LogManager::Flush() {
    output_.flush();
    if (!output_) { throw std::runtime_error("Cannot flush WAL file"); }
    const auto descriptor = ::open(path_.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot open WAL for durable flush: " +
                                 std::to_string(errno));
    }
    const auto sync_result = ::fsync(descriptor);
    const auto sync_error = errno;
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        throw std::runtime_error("Cannot durably flush WAL: " +
                                 std::to_string(sync_result != 0 ? sync_error : errno));
    }
    if (!records_.empty()) { persistent_lsn_ = records_.back().GetLsn(); }
}

void LogManager::Reset() {
    Flush();
    output_.close();
    if (!output_) { throw std::runtime_error("Cannot close WAL for reset"); }
    std::filesystem::resize_file(path_, 0);
    output_.open(path_, std::ios::binary | std::ios::app);
    if (!output_) { throw std::runtime_error("Cannot reopen reset WAL"); }
    records_.clear();
    next_lsn_ = 0;
    persistent_lsn_.reset();
    Flush();
}

}  // namespace udb
