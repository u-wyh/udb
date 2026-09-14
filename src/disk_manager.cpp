#include "udb/disk_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <unistd.h>

namespace udb {
namespace {

constexpr auto kPageBytes = static_cast<std::streamoff>(PAGE_SIZE);
constexpr auto kMaxPages = std::numeric_limits<std::streamoff>::max() / kPageBytes;
constexpr std::uint64_t kPageLsnMagic = 0x314e534c50424455ULL;  // "UDBPLSN1"
constexpr std::uint32_t kPageLsnVersion = 1;
constexpr std::size_t kPageLsnHeaderSize = 16;
constexpr lsn_t kNoPageLsn = std::numeric_limits<lsn_t>::max();

void Put(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

std::uint64_t Get(const std::vector<unsigned char>& bytes, std::size_t offset,
                  std::size_t width) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        throw std::runtime_error("Truncated page LSN sidecar");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

void DurableSync(const std::filesystem::path& path, const char* description) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::runtime_error(std::string("Cannot open ") + description +
                                 " for durable sync: " + std::to_string(errno));
    }
    const auto sync_result = ::fsync(descriptor);
    const auto sync_error = errno;
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        throw std::runtime_error(std::string("Cannot durably sync ") + description + ": " +
                                 std::to_string(sync_result != 0 ? sync_error : errno));
    }
}

}  // namespace

DiskManager::DiskManager(const std::filesystem::path& path)
    : path_(path), page_lsn_path_(GetPageLsnPath(path)) {
    if (!std::filesystem::exists(path)) {
        std::ofstream created(path, std::ios::binary | std::ios::app);
        created.close();
        if (!created) {
            throw std::runtime_error("Cannot create database file");
        }
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Database path must be a regular file");
    }
    file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        throw std::runtime_error("Cannot open database file");
    }
    file_.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(file_.tellg());
    if (size < 0 || size % kPageBytes != 0) {
        throw std::runtime_error("Database file size must be a multiple of PAGE_SIZE");
    }
    page_count_ = size / kPageBytes;
    OpenPageLsnStore();
}

std::filesystem::path DiskManager::GetPageLsnPath(
    const std::filesystem::path& data_path) {
    auto result = data_path;
    result += ".plsn";
    return result;
}

void DiskManager::OpenPageLsnStore() {
    if (!std::filesystem::exists(page_lsn_path_)) {
        std::vector<unsigned char> initial;
        Put(initial, kPageLsnMagic, 8);
        Put(initial, kPageLsnVersion, 4);
        Put(initial, 0, 4);
        std::ofstream created(page_lsn_path_, std::ios::binary | std::ios::trunc);
        created.write(reinterpret_cast<const char*>(initial.data()),
                      static_cast<std::streamsize>(initial.size()));
        created.close();
        if (!created) { throw std::runtime_error("Cannot create page LSN sidecar"); }
        DurableSync(page_lsn_path_, "page LSN sidecar");
    }
    if (!std::filesystem::is_regular_file(page_lsn_path_)) {
        throw std::runtime_error("Page LSN sidecar must be a regular file");
    }
    const auto size = std::filesystem::file_size(page_lsn_path_);
    if (size < kPageLsnHeaderSize || (size - kPageLsnHeaderSize) % 8 != 0 ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("Invalid page LSN sidecar size");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream input(page_lsn_path_, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot read page LSN sidecar");
    }
    if (Get(bytes, 0, 8) != kPageLsnMagic || Get(bytes, 8, 4) != kPageLsnVersion) {
        throw std::runtime_error("Unsupported page LSN sidecar format");
    }
    const auto entries = (bytes.size() - kPageLsnHeaderSize) / 8;
    if (entries > static_cast<std::size_t>(page_count_)) {
        throw std::runtime_error("Page LSN sidecar extends beyond the data file");
    }
    page_lsns_.reserve(static_cast<std::size_t>(page_count_));
    for (std::size_t index = 0; index < entries; ++index) {
        const auto value = Get(bytes, kPageLsnHeaderSize + index * 8, 8);
        page_lsns_.push_back(value == kNoPageLsn ? std::nullopt
                                                 : std::optional<lsn_t>(value));
    }
    page_lsn_file_.open(page_lsn_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!page_lsn_file_) { throw std::runtime_error("Cannot open page LSN sidecar"); }
    while (page_lsns_.size() < static_cast<std::size_t>(page_count_)) {
        WritePageLsnUnlocked(static_cast<page_id_t>(page_lsns_.size()), std::nullopt);
    }
}

std::streamoff DiskManager::Offset(page_id_t page_id) const {
    if (!IsPageAllocatedUnlocked(page_id)) {
        throw std::out_of_range("Page ID is not allocated");
    }
    return static_cast<std::streamoff>(page_id) * kPageBytes;
}

page_id_t DiskManager::AllocatePage() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!free_pages_.empty()) {
        const auto page_id = *free_pages_.begin();
        WriteAt(page_id, Page{});
        SyncDataUnlocked();
        WritePageLsnUnlocked(page_id, std::nullopt);
        free_pages_.erase(free_pages_.begin());
        return page_id;
    }
    if (page_count_ >= kMaxPages || page_count_ == std::numeric_limits<page_id_t>::max()) {
        throw std::overflow_error("Database file has reached the page limit");
    }
    const auto page_id = page_count_;
    WriteAt(page_id, Page{});
    ++page_count_;
    SyncDataUnlocked();
    WritePageLsnUnlocked(page_id, std::nullopt);
    return page_id;
}

void DiskManager::WriteAt(page_id_t page_id, const Page& page) {
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_id) * kPageBytes);
    file_.write(page.data.data(), static_cast<std::streamsize>(PAGE_SIZE));
    file_.flush();
    if (!file_) {
        throw std::runtime_error("Cannot write complete page");
    }
}

void DiskManager::WritePage(page_id_t page_id, const Page& page,
                            std::optional<lsn_t> page_lsn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Offset(page_id);
    WriteAt(page_id, page);
    if (page_lsn) {
        SyncDataUnlocked();
        WritePageLsnUnlocked(page_id, page_lsn);
    }
}

std::optional<lsn_t> DiskManager::GetPageLsn(page_id_t page_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!IsPageAllocatedUnlocked(page_id)) { throw std::out_of_range("Page ID is not allocated"); }
    return page_lsns_.at(static_cast<std::size_t>(page_id));
}

void DiskManager::SetPageLsn(page_id_t page_id, lsn_t page_lsn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!IsPageAllocatedUnlocked(page_id)) { throw std::out_of_range("Page ID is not allocated"); }
    SyncDataUnlocked();
    WritePageLsnUnlocked(page_id, page_lsn);
}

Page DiskManager::ReadPage(page_id_t page_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto offset = Offset(page_id);
    Page page;
    file_.clear();
    file_.seekg(offset);
    file_.read(page.data.data(), static_cast<std::streamsize>(PAGE_SIZE));
    if (!file_ || file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        throw std::runtime_error("Cannot read complete page");
    }
    return page;
}

void DiskManager::Sync() {
    const std::lock_guard<std::mutex> lock(mutex_);
    SyncDataUnlocked();
    SyncPageLsnUnlocked();
}

void DiskManager::SyncDataUnlocked() {
    file_.flush();
    if (!file_) { throw std::runtime_error("Cannot flush database file"); }
    DurableSync(path_, "database");
}

void DiskManager::SyncPageLsnUnlocked() {
    page_lsn_file_.flush();
    if (!page_lsn_file_) { throw std::runtime_error("Cannot flush page LSN sidecar"); }
    DurableSync(page_lsn_path_, "page LSN sidecar");
}

void DiskManager::WritePageLsnUnlocked(page_id_t page_id,
                                       std::optional<lsn_t> page_lsn) {
    if (page_id < 0 || page_id >= page_count_) {
        throw std::out_of_range("Page LSN ID is outside the database file");
    }
    const auto index = static_cast<std::size_t>(page_id);
    if (index > page_lsns_.size()) {
        throw std::logic_error("Page LSN sidecar has a gap");
    }
    std::vector<unsigned char> encoded;
    Put(encoded, page_lsn.value_or(kNoPageLsn), 8);
    page_lsn_file_.clear();
    page_lsn_file_.seekp(static_cast<std::streamoff>(kPageLsnHeaderSize + index * 8));
    page_lsn_file_.write(reinterpret_cast<const char*>(encoded.data()), 8);
    if (!page_lsn_file_) { throw std::runtime_error("Cannot write page LSN sidecar"); }
    if (index == page_lsns_.size()) { page_lsns_.push_back(page_lsn); }
    else { page_lsns_[index] = page_lsn; }
    SyncPageLsnUnlocked();
}

bool DiskManager::IsPageAllocated(page_id_t page_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return IsPageAllocatedUnlocked(page_id);
}

bool DiskManager::IsPageAllocatedUnlocked(page_id_t page_id) const {
    return page_id >= 0 && page_id < page_count_ && free_pages_.count(page_id) == 0;
}

page_id_t DiskManager::GetPageCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return page_count_;
}

page_id_t DiskManager::GetNextPageId() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return free_pages_.empty() ? page_count_ : *free_pages_.begin();
}

std::set<page_id_t> DiskManager::GetFreePageIds() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return free_pages_;
}

void DiskManager::DeallocatePage(page_id_t page_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (page_id < 0 || page_id >= page_count_) {
        throw std::out_of_range("Page ID is outside the database file");
    }
    if (!free_pages_.insert(page_id).second) {
        throw std::logic_error("Page is already free");
    }
}

void DiskManager::RestorePage(page_id_t page_id, const Page& page) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = free_pages_.find(page_id);
    if (found == free_pages_.end()) { throw std::logic_error("Page is not free"); }
    WriteAt(page_id, page);
    free_pages_.erase(found);
}

void DiskManager::RestoreFreePageIds(const std::vector<page_id_t>& page_ids) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::set<page_id_t> restored;
    for (const auto page_id : page_ids) {
        if (page_id < 0 || page_id >= page_count_) {
            throw std::runtime_error("Metadata free page ID is out of range");
        }
        if (!restored.insert(page_id).second) {
            throw std::runtime_error("Duplicate metadata free page ID");
        }
    }
    free_pages_ = std::move(restored);
}

void DiskManager::RecoveryWritePage(page_id_t page_id, const Page& page,
                                    std::optional<lsn_t> page_lsn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (page_id < 0 || page_id >= kMaxPages) {
        throw std::out_of_range("Recovery page ID is outside the database file");
    }
    while (page_count_ <= page_id) {
        WriteAt(page_count_, Page{});
        ++page_count_;
        SyncDataUnlocked();
        WritePageLsnUnlocked(page_count_ - 1, std::nullopt);
    }
    WriteAt(page_id, page);
    if (page_lsn) {
        SyncDataUnlocked();
        WritePageLsnUnlocked(page_id, page_lsn);
    }
}

void DiskManager::ApplyRecoveryPageStates(const std::map<page_id_t, bool>& allocated) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [page_id, is_allocated] : allocated) {
        if (page_id < 0 || page_id >= page_count_) {
            throw std::runtime_error("Recovery allocation state is out of range");
        }
        if (is_allocated) {
            free_pages_.erase(page_id);
        } else {
            free_pages_.insert(page_id);
        }
    }
}

}  // namespace udb
