#include "udb/disk_manager.h"

#include <limits>
#include <stdexcept>

namespace udb {
namespace {

constexpr auto kPageBytes = static_cast<std::streamoff>(PAGE_SIZE);
constexpr auto kMaxPages = std::numeric_limits<std::streamoff>::max() / kPageBytes;

}  // namespace

DiskManager::DiskManager(const std::filesystem::path& path) {
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
    if (page_id < 0 || page_id >= page_count_) {
        throw std::out_of_range("Page ID is not allocated");
    }
    return static_cast<std::streamoff>(page_id) * kPageBytes;
}

page_id_t DiskManager::AllocatePage() {
    if (page_count_ >= kMaxPages || page_count_ == std::numeric_limits<page_id_t>::max()) {
        throw std::overflow_error("Database file has reached the page limit");
    }
    const Page page;
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_count_) * kPageBytes);
    file_.write(page.data.data(), static_cast<std::streamsize>(PAGE_SIZE));
    file_.flush();
    if (!file_) {
        throw std::runtime_error("Cannot allocate page");
    }
    return page_count_++;
}

void DiskManager::WritePage(page_id_t page_id, const Page& page) {
    const auto offset = Offset(page_id);
    file_.clear();
    file_.seekp(offset);
    file_.write(page.data.data(), static_cast<std::streamsize>(PAGE_SIZE));
    file_.flush();
    if (!file_) {
        throw std::runtime_error("Cannot write page");
    }
}

Page DiskManager::ReadPage(page_id_t page_id) {
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

}  // namespace udb
