#pragma once

#include "udb/page.h"

#include <mutex>
#include <shared_mutex>

namespace udb {

class BufferPoolManager;

class ReadPageGuard {
public:
    ReadPageGuard() = default;
    ~ReadPageGuard();
    ReadPageGuard(const ReadPageGuard&) = delete;
    ReadPageGuard& operator=(const ReadPageGuard&) = delete;
    ReadPageGuard(ReadPageGuard&& other) noexcept;
    ReadPageGuard& operator=(ReadPageGuard&& other);

    page_id_t GetPageId() const;
    const Page& GetPage() const;
    bool IsValid() const { return page_ != nullptr; }
    void Drop();

private:
    friend class BufferPoolManager;
    ReadPageGuard(BufferPoolManager& pool, page_id_t page_id, Page* page,
                  std::shared_lock<std::shared_mutex>&& latch)
        : pool_(&pool), page_id_(page_id), page_(page), latch_(std::move(latch)) {}
    void Take(ReadPageGuard&& other) noexcept;

    BufferPoolManager* pool_ = nullptr;
    page_id_t page_id_ = -1;
    Page* page_ = nullptr;
    std::shared_lock<std::shared_mutex> latch_;
};

class WritePageGuard {
public:
    WritePageGuard() = default;
    ~WritePageGuard();
    WritePageGuard(const WritePageGuard&) = delete;
    WritePageGuard& operator=(const WritePageGuard&) = delete;
    WritePageGuard(WritePageGuard&& other) noexcept;
    WritePageGuard& operator=(WritePageGuard&& other);

    page_id_t GetPageId() const;
    Page& GetPage();
    const Page& GetPage() const;
    bool IsValid() const { return page_ != nullptr; }
    void Drop();

private:
    friend class BufferPoolManager;
    WritePageGuard(BufferPoolManager& pool, page_id_t page_id, Page* page,
                   std::unique_lock<std::shared_mutex>&& latch)
        : pool_(&pool), page_id_(page_id), page_(page), before_image_(*page),
          latch_(std::move(latch)) {}
    void Take(WritePageGuard&& other) noexcept;

    BufferPoolManager* pool_ = nullptr;
    page_id_t page_id_ = -1;
    Page* page_ = nullptr;
    Page before_image_{};
    std::unique_lock<std::shared_mutex> latch_;
};

}  // namespace udb
