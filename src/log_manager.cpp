#include "udb/log_manager.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace udb {

class LogRecordDecoder {
public:
    static LogRecord Make(LogRecordType type, transaction_id_t transaction_id,
                          std::optional<page_id_t> page_id,
                          std::optional<Page> before, std::optional<Page> after,
                          std::optional<timestamp_t> commit_timestamp, lsn_t lsn,
                          std::optional<lsn_t> prev_lsn,
                          std::optional<lsn_t> undo_next_lsn,
                          std::optional<CompensationType> compensation_type) {
        LogRecord record(type, transaction_id, page_id, std::move(before), std::move(after),
                         commit_timestamp, prev_lsn, undo_next_lsn, compensation_type);
        record.lsn_ = lsn;
        return record;
    }
};

namespace {

constexpr std::uint64_t kMagic = 0x31304c4157424455ULL;  // "UDBWAL01"
constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kLegacyHeaderSize = 48;
constexpr std::size_t kHeaderSize = 64;
constexpr std::uint64_t kMissing = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kSequenceMagic = 0x31514553424455ULL;  // "UDBSEQ1"
constexpr std::uint32_t kSequenceVersion = 1;
constexpr std::uint64_t kCheckpointMagic = 0x31545043424455ULL;  // "UDBCP1"
constexpr std::uint32_t kCheckpointVersion = 1;

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
        case LogRecordType::Compensation: return PAGE_SIZE;
    }
    throw std::runtime_error("Unknown WAL record type");
}

LogRecordType DecodeType(std::uint64_t value, std::uint32_t version) {
    const auto maximum = version == kLegacyVersion ? LogRecordType::Abort
                                                    : LogRecordType::Compensation;
    if (value > static_cast<std::uint64_t>(maximum)) {
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
    Write(bytes, record.GetCompensationType()
                     ? static_cast<std::uint8_t>(*record.GetCompensationType())
                     : std::numeric_limits<std::uint8_t>::max(), 1);
    Write(bytes, 0, 2);
    Write(bytes, record.GetLsn(), 8);
    Write(bytes, record.GetTransactionId(), 8);
    Write(bytes, record.GetCommitTimestamp()
                     ? *record.GetCommitTimestamp()
                     : (record.GetPageId() ? static_cast<std::uint64_t>(*record.GetPageId())
                                           : kMissing), 8);
    Write(bytes, record.GetPrevLsn().value_or(kMissing), 8);
    Write(bytes, record.GetUndoNextLsn().value_or(kMissing), 8);
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
    std::optional<lsn_t> expected_lsn;
    std::map<transaction_id_t, lsn_t> transaction_last_lsns;
    while (position < file.size()) {
        if (file.size() - position < 12) { break; }
        if (Read(file, position, 8) != kMagic) {
            throw std::runtime_error("Invalid WAL record header");
        }
        const auto version = static_cast<std::uint32_t>(Read(file, position + 8, 4));
        if (version != kLegacyVersion && version != kVersion) {
            throw std::runtime_error("Unsupported WAL record version");
        }
        const auto header_size = version == kLegacyVersion ? kLegacyHeaderSize : kHeaderSize;
        if (file.size() - position < header_size) { break; }
        const auto size = Read(file, position + 12, 4);
        const auto type = DecodeType(Read(file, position + 20, 1), version);
        const auto expected_size = header_size + PayloadSize(type);
        if (size != expected_size) { throw std::runtime_error("Invalid WAL record size"); }
        if (size > file.size() - position) { break; }
        std::vector<unsigned char> encoded(file.begin() + static_cast<std::ptrdiff_t>(position),
                                           file.begin() + static_cast<std::ptrdiff_t>(position + size));
        const auto stored_checksum = Read(encoded, 16, 4);
        Overwrite(encoded, 16, 0, 4);
        if (Checksum(encoded) != stored_checksum) { throw std::runtime_error("WAL checksum mismatch"); }
        const auto lsn = Read(encoded, 24, 8);
        if (expected_lsn && lsn != *expected_lsn) {
            throw std::runtime_error("Non-contiguous WAL LSN sequence");
        }
        if (lsn == std::numeric_limits<lsn_t>::max()) {
            throw std::runtime_error("WAL LSN sequence overflow");
        }
        expected_lsn = lsn + 1;
        const auto transaction_id = Read(encoded, 32, 8);
        const auto encoded_page_id = Read(encoded, 40, 8);
        std::optional<lsn_t> prev_lsn;
        std::optional<lsn_t> undo_next_lsn;
        std::optional<CompensationType> compensation_type;
        if (version == kVersion) {
            const auto encoded_prev = Read(encoded, 48, 8);
            const auto encoded_undo = Read(encoded, 56, 8);
            if (encoded_prev != kMissing) { prev_lsn = encoded_prev; }
            if (encoded_undo != kMissing) { undo_next_lsn = encoded_undo; }
            if (type == LogRecordType::Compensation) {
                const auto encoded_compensation = Read(encoded, 21, 1);
                if (encoded_compensation > static_cast<std::uint64_t>(CompensationType::PageFree)) {
                    throw std::runtime_error("Invalid WAL compensation type");
                }
                compensation_type = static_cast<CompensationType>(encoded_compensation);
            }
        } else if (type != LogRecordType::Begin) {
            const auto previous = transaction_last_lsns.find(transaction_id);
            if (previous != transaction_last_lsns.end()) { prev_lsn = previous->second; }
        }
        std::optional<page_id_t> page_id;
        std::optional<timestamp_t> commit_timestamp;
        if (type == LogRecordType::Commit &&
            encoded_page_id != kMissing) {
            commit_timestamp = encoded_page_id;
        } else if (encoded_page_id != kMissing) {
            if (encoded_page_id > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
                throw std::runtime_error("Invalid WAL page ID");
            }
            page_id = static_cast<page_id_t>(encoded_page_id);
        }
        std::optional<Page> before;
        std::optional<Page> after;
        std::size_t payload = header_size;
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
        if (type == LogRecordType::Compensation) {
            after.emplace();
            std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(payload), PAGE_SIZE,
                        after->data.begin());
        }
        auto record = LogRecordDecoder::Make(type, transaction_id, page_id,
                                             std::move(before), std::move(after),
                                             commit_timestamp, lsn, prev_lsn,
                                             undo_next_lsn, compensation_type);
        record.Validate();
        records.push_back(std::move(record));
        if (type == LogRecordType::Begin) {
            transaction_last_lsns[transaction_id] = lsn;
        } else if (type == LogRecordType::Commit || type == LogRecordType::Abort) {
            transaction_last_lsns.erase(transaction_id);
        } else {
            transaction_last_lsns[transaction_id] = lsn;
        }
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
LogRecord LogRecord::Commit(transaction_id_t transaction_id,
                            std::optional<timestamp_t> commit_timestamp) {
    return LogRecord(LogRecordType::Commit, transaction_id, std::nullopt,
                     std::nullopt, std::nullopt, commit_timestamp);
}
LogRecord LogRecord::Abort(transaction_id_t transaction_id) {
    return LogRecord(LogRecordType::Abort, transaction_id);
}
LogRecord LogRecord::Compensation(transaction_id_t transaction_id, page_id_t page_id,
                                  CompensationType compensation_type, const Page& after,
                                  std::optional<lsn_t> undo_next_lsn) {
    return LogRecord(LogRecordType::Compensation, transaction_id, page_id,
                     std::nullopt, after, std::nullopt, std::nullopt,
                     undo_next_lsn, compensation_type);
}

void LogRecord::Validate() const {
    const bool has_page = page_id_.has_value();
    const bool has_before = before_image_.has_value();
    const bool has_after = after_image_.has_value();
    if (type_ != LogRecordType::Commit && commit_timestamp_) {
        throw std::invalid_argument("Only COMMIT WAL records carry a commit timestamp");
    }
    if (commit_timestamp_ && (*commit_timestamp_ >> 63U) != 0) {
        throw std::invalid_argument("COMMIT WAL timestamp must be a committed timestamp");
    }
    if (undo_next_lsn_ && type_ != LogRecordType::Compensation) {
        throw std::invalid_argument("Only CLR WAL records carry undoNextLSN");
    }
    if (compensation_type_.has_value() != (type_ == LogRecordType::Compensation)) {
        throw std::invalid_argument("WAL compensation action does not match record type");
    }
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
        case LogRecordType::Compensation:
            if (!has_page || has_before || !has_after) {
                throw std::invalid_argument("Invalid CLR WAL record");
            }
            return;
    }
    throw std::invalid_argument("Unknown WAL record type");
}

LogManager::LogManager(const std::filesystem::path& path)
    : path_(path), sequence_path_(GetSequencePath(path)),
      checkpoint_path_(GetCheckpointPath(path)) {
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
    lsn_t sequence_floor = 0;
    if (std::filesystem::exists(sequence_path_)) {
        const auto sequence = ReadFile(sequence_path_);
        if (sequence.size() != 24 || Read(sequence, 0, 8) != kSequenceMagic ||
            Read(sequence, 8, 4) != kSequenceVersion) {
            throw std::runtime_error("Unsupported WAL sequence sidecar format");
        }
        sequence_floor = Read(sequence, 16, 8);
    }
    next_lsn_ = records_.empty() ? sequence_floor : records_.back().GetLsn() + 1;
    if (!records_.empty()) { persistent_lsn_ = records_.back().GetLsn(); }
    for (const auto& record : records_) {
        if (record.GetType() == LogRecordType::Begin) {
            transaction_last_lsns_[record.GetTransactionId()] = record.GetLsn();
        } else if (record.GetType() == LogRecordType::Commit ||
                   record.GetType() == LogRecordType::Abort) {
            transaction_last_lsns_.erase(record.GetTransactionId());
        } else {
            transaction_last_lsns_[record.GetTransactionId()] = record.GetLsn();
        }
    }
    output_.open(path, std::ios::binary | std::ios::app);
    if (!output_) { throw std::runtime_error("Cannot open WAL file"); }
}

std::filesystem::path LogManager::GetSequencePath(
    const std::filesystem::path& wal_path) {
    auto result = wal_path;
    result += ".seq";
    return result;
}

std::filesystem::path LogManager::GetCheckpointPath(
    const std::filesystem::path& wal_path) {
    auto result = wal_path;
    result += ".ckpt";
    return result;
}

void LogManager::PersistSequenceFloor() {
    std::vector<unsigned char> bytes;
    Write(bytes, kSequenceMagic, 8);
    Write(bytes, kSequenceVersion, 4);
    Write(bytes, 0, 4);
    Write(bytes, next_lsn_, 8);
    std::ofstream output(sequence_path_, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) { throw std::runtime_error("Cannot write WAL sequence sidecar"); }
    const auto descriptor = ::open(sequence_path_.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot durably sync WAL sequence sidecar");
    }
    const auto sync_error = ::fsync(descriptor) != 0;
    const auto close_error = ::close(descriptor) != 0;
    if (sync_error || close_error) {
        throw std::runtime_error("Cannot durably sync WAL sequence sidecar");
    }
}

void LogManager::EnsureNextLsn(lsn_t minimum) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!records_.empty()) { return; }
    if (minimum > next_lsn_) {
        next_lsn_ = minimum;
        PersistSequenceFloor();
    }
}

lsn_t LogManager::Append(LogRecord record) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (next_lsn_ == std::numeric_limits<lsn_t>::max()) {
        throw std::overflow_error("WAL LSN limit reached");
    }
    record.lsn_ = next_lsn_;
    if (record.type_ == LogRecordType::Begin) {
        record.prev_lsn_.reset();
    } else if (!record.prev_lsn_) {
        const auto previous = transaction_last_lsns_.find(record.transaction_id_);
        if (previous != transaction_last_lsns_.end()) { record.prev_lsn_ = previous->second; }
    }
    record.Validate();
    const auto encoded = Encode(record);
    records_.reserve(records_.size() + 1);
    output_.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
    if (!output_) { throw std::runtime_error("Cannot append WAL record"); }
    records_.push_back(std::move(record));
    const auto appended_lsn = next_lsn_++;
    const auto& appended = records_.back();
    if (appended.GetType() == LogRecordType::Commit ||
        appended.GetType() == LogRecordType::Abort) {
        transaction_last_lsns_.erase(appended.GetTransactionId());
    } else {
        transaction_last_lsns_[appended.GetTransactionId()] = appended_lsn;
    }
    return appended_lsn;
}

void LogManager::Flush() {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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

bool LogManager::HasActiveTransactions() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::set<transaction_id_t> active;
    for (const auto& record : records_) {
        switch (record.GetType()) {
            case LogRecordType::Begin:
                active.insert(record.GetTransactionId());
                break;
            case LogRecordType::Commit:
            case LogRecordType::Abort:
                active.erase(record.GetTransactionId());
                break;
            case LogRecordType::PageWrite:
            case LogRecordType::PageAllocate:
            case LogRecordType::PageFree:
            case LogRecordType::Compensation:
                break;
        }
    }
    return !active.empty();
}

std::map<transaction_id_t, lsn_t> LogManager::GetActiveTransactionTable() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::map<transaction_id_t, lsn_t> active;
    for (const auto& record : records_) {
        if (record.GetType() == LogRecordType::Begin) {
            active.insert_or_assign(record.GetTransactionId(), record.GetLsn());
        } else if (record.GetType() == LogRecordType::Commit ||
                   record.GetType() == LogRecordType::Abort) {
            active.erase(record.GetTransactionId());
        } else {
            const auto found = active.find(record.GetTransactionId());
            if (found != active.end()) { found->second = record.GetLsn(); }
        }
    }
    return active;
}

void LogManager::WriteCheckpoint(const LogCheckpoint& checkpoint) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<unsigned char> bytes;
    Write(bytes, kCheckpointMagic, 8);
    Write(bytes, kCheckpointVersion, 4);
    Write(bytes, 0, 4);
    Write(bytes, checkpoint.checkpoint_lsn, 8);
    Write(bytes, checkpoint.transaction_table.size(), 8);
    Write(bytes, checkpoint.dirty_page_table.size(), 8);
    for (const auto& [transaction_id, last_lsn] : checkpoint.transaction_table) {
        Write(bytes, transaction_id, 8);
        Write(bytes, last_lsn, 8);
    }
    for (const auto& [page_id, rec_lsn] : checkpoint.dirty_page_table) {
        if (page_id < 0) { throw std::invalid_argument("Checkpoint page ID is invalid"); }
        Write(bytes, static_cast<std::uint64_t>(page_id), 8);
        Write(bytes, rec_lsn, 8);
    }
    Overwrite(bytes, 12, Checksum(bytes), 4);
    auto temporary = checkpoint_path_;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) { throw std::runtime_error("Cannot write WAL checkpoint sidecar"); }
    const auto descriptor = ::open(temporary.c_str(), O_RDONLY);
    if (descriptor < 0) { throw std::runtime_error("Cannot open WAL checkpoint sidecar"); }
    const auto sync_error = ::fsync(descriptor) != 0;
    const auto close_error = ::close(descriptor) != 0;
    if (sync_error || close_error) {
        throw std::runtime_error("Cannot durably sync WAL checkpoint sidecar");
    }
    std::filesystem::rename(temporary, checkpoint_path_);
    auto parent = checkpoint_path_.parent_path();
    if (parent.empty()) { parent = "."; }
    const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) { throw std::runtime_error("Cannot open WAL checkpoint directory"); }
    const auto directory_sync_error = ::fsync(directory) != 0;
    const auto directory_close_error = ::close(directory) != 0;
    if (directory_sync_error || directory_close_error) {
        throw std::runtime_error("Cannot durably sync WAL checkpoint directory");
    }
}

std::optional<LogCheckpoint> LogManager::ReadCheckpoint() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!std::filesystem::exists(checkpoint_path_)) { return std::nullopt; }
    auto bytes = ReadFile(checkpoint_path_);
    if (bytes.size() < 40 || Read(bytes, 0, 8) != kCheckpointMagic ||
        Read(bytes, 8, 4) != kCheckpointVersion) {
        throw std::runtime_error("Unsupported WAL checkpoint sidecar format");
    }
    const auto checksum = Read(bytes, 12, 4);
    Overwrite(bytes, 12, 0, 4);
    if (Checksum(bytes) != checksum) {
        throw std::runtime_error("WAL checkpoint sidecar checksum mismatch");
    }
    LogCheckpoint checkpoint;
    checkpoint.checkpoint_lsn = Read(bytes, 16, 8);
    const auto transaction_count = Read(bytes, 24, 8);
    const auto dirty_count = Read(bytes, 32, 8);
    if (transaction_count > (bytes.size() - 40) / 16 ||
        dirty_count > (bytes.size() - 40) / 16 - transaction_count ||
        40 + 16 * (transaction_count + dirty_count) != bytes.size()) {
        throw std::runtime_error("Invalid WAL checkpoint sidecar length");
    }
    std::size_t offset = 40;
    for (std::uint64_t i = 0; i < transaction_count; ++i, offset += 16) {
        if (!checkpoint.transaction_table.emplace(Read(bytes, offset, 8),
                                                  Read(bytes, offset + 8, 8)).second) {
            throw std::runtime_error("Duplicate checkpoint transaction ID");
        }
    }
    for (std::uint64_t i = 0; i < dirty_count; ++i, offset += 16) {
        const auto page = Read(bytes, offset, 8);
        if (page > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
            throw std::runtime_error("Invalid checkpoint page ID");
        }
        if (!checkpoint.dirty_page_table.emplace(static_cast<page_id_t>(page),
                                                 Read(bytes, offset + 8, 8)).second) {
            throw std::runtime_error("Duplicate checkpoint page ID");
        }
    }
    return checkpoint;
}

std::size_t LogManager::TruncateForCheckpoint(const LogCheckpoint& checkpoint) {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (checkpoint.checkpoint_lsn > next_lsn_) {
        throw std::invalid_argument("Checkpoint LSN is ahead of the WAL");
    }
    if (records_.empty()) { return 0; }
    lsn_t first_required = checkpoint.checkpoint_lsn;
    for (const auto& [page_id, rec_lsn] : checkpoint.dirty_page_table) {
        static_cast<void>(page_id);
        first_required = std::min(first_required, rec_lsn);
    }
    std::map<transaction_id_t, lsn_t> begin_lsns;
    for (const auto& record : records_) {
        if (record.GetType() == LogRecordType::Begin) {
            begin_lsns.emplace(record.GetTransactionId(), record.GetLsn());
        }
    }
    for (const auto& [transaction_id, last_lsn] : checkpoint.transaction_table) {
        static_cast<void>(last_lsn);
        const auto begin = begin_lsns.find(transaction_id);
        if (begin == begin_lsns.end()) {
            throw std::invalid_argument("Checkpoint transaction has no WAL BEGIN");
        }
        first_required = std::min(first_required, begin->second);
    }
    // Never retain the tail of a transaction without its BEGIN. Lowering the
    // boundary can expose another interleaved transaction, so close to a fixed point.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& record : records_) {
            if (record.GetLsn() < first_required) { continue; }
            const auto begin = begin_lsns.find(record.GetTransactionId());
            if (begin == begin_lsns.end()) {
                throw std::runtime_error("WAL transaction has no BEGIN during truncation");
            }
            if (begin->second < first_required) {
                first_required = begin->second;
                changed = true;
            }
        }
    }
    const auto keep = std::lower_bound(
        records_.begin(), records_.end(), first_required,
        [](const LogRecord& record, lsn_t lsn) { return record.GetLsn() < lsn; });
    const auto removed = static_cast<std::size_t>(std::distance(records_.begin(), keep));
    if (removed == 0) { return 0; }

    std::vector<unsigned char> bytes;
    for (auto current = keep; current != records_.end(); ++current) {
        const auto encoded = Encode(*current);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    auto temporary = path_;
    temporary += ".tmp";
    std::ofstream replacement(temporary, std::ios::binary | std::ios::trunc);
    replacement.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
    replacement.close();
    if (!replacement) { throw std::runtime_error("Cannot write truncated WAL"); }
    const auto descriptor = ::open(temporary.c_str(), O_RDONLY);
    if (descriptor < 0) { throw std::runtime_error("Cannot open truncated WAL"); }
    const auto sync_error = ::fsync(descriptor) != 0;
    const auto close_error = ::close(descriptor) != 0;
    if (sync_error || close_error) { throw std::runtime_error("Cannot durably sync truncated WAL"); }

    output_.close();
    if (!output_) { throw std::runtime_error("Cannot close WAL before truncation"); }
    std::filesystem::rename(temporary, path_);
    output_.open(path_, std::ios::binary | std::ios::app);
    if (!output_) { throw std::runtime_error("Cannot reopen truncated WAL"); }
    records_.erase(records_.begin(), keep);
    transaction_last_lsns_.clear();
    for (const auto& record : records_) {
        if (record.GetType() == LogRecordType::Commit ||
            record.GetType() == LogRecordType::Abort) {
            transaction_last_lsns_.erase(record.GetTransactionId());
        } else {
            transaction_last_lsns_.insert_or_assign(record.GetTransactionId(), record.GetLsn());
        }
    }
    persistent_lsn_ = records_.empty()
        ? std::nullopt : std::optional<lsn_t>(records_.back().GetLsn());
    PersistSequenceFloor();
    auto parent = path_.parent_path();
    if (parent.empty()) { parent = "."; }
    const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) { throw std::runtime_error("Cannot open WAL directory after truncation"); }
    const auto directory_sync_error = ::fsync(directory) != 0;
    const auto directory_close_error = ::close(directory) != 0;
    if (directory_sync_error || directory_close_error) {
        throw std::runtime_error("Cannot durably sync WAL directory after truncation");
    }
    return removed;
}

void LogManager::Reset() {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (HasActiveTransactions()) {
        throw std::logic_error("Cannot reset WAL with an active transaction");
    }
    Flush();
    output_.close();
    if (!output_) { throw std::runtime_error("Cannot close WAL for reset"); }
    std::filesystem::resize_file(path_, 0);
    output_.open(path_, std::ios::binary | std::ios::app);
    if (!output_) { throw std::runtime_error("Cannot reopen reset WAL"); }
    records_.clear();
    transaction_last_lsns_.clear();
    PersistSequenceFloor();
    persistent_lsn_.reset();
    Flush();
}

lsn_t LogManager::GetNextLsn() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return next_lsn_;
}

std::optional<lsn_t> LogManager::GetPersistentLsn() const {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return persistent_lsn_;
}

}  // namespace udb
