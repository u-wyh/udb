#pragma once

#include "udb/page.h"

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
    ReadPageGuard(BufferPoolManager& pool, page_id_t page_id, Page* page)
        : pool_(&pool), page_id_(page_id), page_(page) {}
    void Take(ReadPageGuard&& other) noexcept;

    BufferPoolManager* pool_ = nullptr;
    page_id_t page_id_ = -1;
    Page* page_ = nullptr;
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
    WritePageGuard(BufferPoolManager& pool, page_id_t page_id, Page* page)
        : pool_(&pool), page_id_(page_id), page_(page) {}
    void Take(WritePageGuard&& other) noexcept;

    BufferPoolManager* pool_ = nullptr;
    page_id_t page_id_ = -1;
    Page* page_ = nullptr;
};

}  // namespace udb
