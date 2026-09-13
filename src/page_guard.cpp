#include "udb/page_guard.h"

#include "udb/buffer_pool_manager.h"

#include <stdexcept>

namespace udb {

ReadPageGuard::~ReadPageGuard() { Drop(); }
ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept { Take(std::move(other)); }

ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) {
    if (this != &other) { Drop(); Take(std::move(other)); }
    return *this;
}

void ReadPageGuard::Take(ReadPageGuard&& other) noexcept {
    pool_ = other.pool_;
    page_id_ = other.page_id_;
    page_ = other.page_;
    latch_ = std::move(other.latch_);
    other.pool_ = nullptr;
    other.page_id_ = -1;
    other.page_ = nullptr;
}

page_id_t ReadPageGuard::GetPageId() const {
    if (!IsValid()) { throw std::logic_error("Read page guard is empty"); }
    return page_id_;
}

const Page& ReadPageGuard::GetPage() const {
    if (!IsValid()) { throw std::logic_error("Read page guard is empty"); }
    return *page_;
}

void ReadPageGuard::Drop() {
    if (!IsValid()) { return; }
    auto* pool = pool_;
    const auto page_id = page_id_;
    pool_ = nullptr;
    page_id_ = -1;
    page_ = nullptr;
    latch_.unlock();
    pool->UnpinPage(page_id, false);
}

WritePageGuard::~WritePageGuard() { Drop(); }
WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept { Take(std::move(other)); }

WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) {
    if (this != &other) { Drop(); Take(std::move(other)); }
    return *this;
}

void WritePageGuard::Take(WritePageGuard&& other) noexcept {
    pool_ = other.pool_;
    page_id_ = other.page_id_;
    page_ = other.page_;
    before_image_ = other.before_image_;
    latch_ = std::move(other.latch_);
    other.pool_ = nullptr;
    other.page_id_ = -1;
    other.page_ = nullptr;
}

page_id_t WritePageGuard::GetPageId() const {
    if (!IsValid()) { throw std::logic_error("Write page guard is empty"); }
    return page_id_;
}

Page& WritePageGuard::GetPage() {
    if (!IsValid()) { throw std::logic_error("Write page guard is empty"); }
    return *page_;
}

const Page& WritePageGuard::GetPage() const {
    if (!IsValid()) { throw std::logic_error("Write page guard is empty"); }
    return *page_;
}

void WritePageGuard::Drop() {
    if (!IsValid()) { return; }
    auto* pool = pool_;
    const auto page_id = page_id_;
    const auto before = before_image_;
    auto* page = page_;
    pool_ = nullptr;
    page_id_ = -1;
    page_ = nullptr;
    pool->CompleteWrite(page_id, before, *page);
    latch_.unlock();
    pool->UnpinPage(page_id, false);
}

}  // namespace udb
