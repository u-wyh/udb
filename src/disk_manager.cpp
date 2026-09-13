#include "udb/disk_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace udb {
namespace {

constexpr auto kPageBytes = static_cast<std::streamoff>(PAGE_SIZE);
constexpr auto kMaxPages = std::numeric_limits<std::streamoff>::max() / kPageBytes;

}  // namespace

DiskManager::DiskManager(const std::filesystem::path& path) : path_(path) {
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
        free_pages_.erase(free_pages_.begin());
        return page_id;
    }
    if (page_count_ >= kMaxPages || page_count_ == std::numeric_limits<page_id_t>::max()) {
        throw std::overflow_error("Database file has reached the page limit");
    }
    WriteAt(page_count_, Page{});
    return page_count_++;
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

void DiskManager::WritePage(page_id_t page_id, const Page& page) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Offset(page_id);
    WriteAt(page_id, page);
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
    file_.flush();
    if (!file_) { throw std::runtime_error("Cannot flush database file"); }
    const auto descriptor = ::open(path_.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot open database for durable sync: " +
                                 std::to_string(errno));
    }
    const auto sync_result = ::fsync(descriptor);
    const auto sync_error = errno;
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        throw std::runtime_error("Cannot durably sync database: " +
                                 std::to_string(sync_result != 0 ? sync_error : errno));
    }
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

void DiskManager::RecoveryWritePage(page_id_t page_id, const Page& page) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (page_id < 0 || page_id >= kMaxPages) {
        throw std::out_of_range("Recovery page ID is outside the database file");
    }
    while (page_count_ <= page_id) {
        WriteAt(page_count_, Page{});
        ++page_count_;
    }
    WriteAt(page_id, page);
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
