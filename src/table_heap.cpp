#include "udb/table_heap.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace udb {

TableHeap::TableHeap(BufferPoolManager& pool) : pool_(pool), first_page_id_(-1) {
    auto page = pool_.NewPageGuard();
    SlottedPage(page.GetPage(), page.GetPageId()).Init();
    first_page_id_ = page.GetPageId();
}

TableHeap::TableHeap(BufferPoolManager& pool, page_id_t first_page_id)
    : pool_(pool), first_page_id_(first_page_id) {
    if (first_page_id < 0) {
        throw std::invalid_argument("Table requires a valid first page ID");
    }
    CollectPageIds();
}

page_id_t TableHeap::GetFirstPageId() const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return first_page_id_;
}

std::vector<page_id_t> TableHeap::CollectPageIds() const {
    std::vector<page_id_t> ids;
    std::unordered_set<page_id_t> seen;
    for (auto id = first_page_id_; id != -1;) {
        if (!seen.insert(id).second) {
            throw std::runtime_error("Table page chain contains a cycle");
        }
        ids.push_back(id);
        auto page = pool_.ReadPage(id);
        SlottedPage view(page.GetPage(), id);
        id = view.GetNextPageId();
    }
    return ids;
}

void TableHeap::RequireMember(page_id_t page_id) const {
    const auto ids = CollectPageIds();
    if (std::find(ids.begin(), ids.end(), page_id) != ids.end()) { return; }
    throw std::out_of_range("RID page is not in this table");
}

RID TableHeap::InsertRecord(const Record& record, TupleMeta meta) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    if (record.Size() > PAGE_SIZE - SlottedPage::HEADER_SIZE - SlottedPage::SLOT_SIZE) {
        throw std::length_error("Record cannot fit in an empty table page");
    }
    const auto ids = CollectPageIds();
    for (const auto id : ids) {
        auto page = pool_.WritePage(id);
        SlottedPage view(page.GetPage(), id);
        if (const auto rid = view.InsertRecord(record, meta)) {
            return *rid;
        }
    }

    RID inserted;
    {
        auto page = pool_.NewPageGuard();
        SlottedPage view(page.GetPage(), page.GetPageId());
        view.Init();
        inserted = view.InsertRecord(record, meta).value();
    }
    // Release the new page before refetching the tail: capacity one is enough.
    // Publish the link only after the new page has been initialized successfully.
    {
        auto tail = pool_.WritePage(ids.back());
        SlottedPage view(tail.GetPage(), tail.GetPageId());
        if (view.GetNextPageId() != -1) {
            throw std::runtime_error("Invalid table tail during append");
        }
        view.SetNextPageId(inserted.page_id);
    }
    return inserted;
}

Record TableHeap::GetRecord(RID rid) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    RequireMember(rid.page_id);
    auto page = pool_.ReadPage(rid.page_id);
    return SlottedPage(page.GetPage(), rid.page_id).GetRecord(rid);
}

TupleMeta TableHeap::GetTupleMeta(RID rid) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    RequireMember(rid.page_id);
    auto page = pool_.ReadPage(rid.page_id);
    return SlottedPage(page.GetPage(), rid.page_id).GetTupleMeta(rid);
}

void TableHeap::SetTupleMeta(RID rid, TupleMeta meta) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    RequireMember(rid.page_id);
    auto page = pool_.WritePage(rid.page_id);
    SlottedPage(page.GetPage(), rid.page_id).SetTupleMeta(rid, meta);
}

bool TableHeap::UpdateRecord(RID rid, const Record& record) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    RequireMember(rid.page_id);
    auto page = pool_.WritePage(rid.page_id);
    return SlottedPage(page.GetPage(), rid.page_id).UpdateRecord(rid, record);
}

void TableHeap::DeleteRecord(RID rid) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    RequireMember(rid.page_id);
    auto page = pool_.WritePage(rid.page_id);
    SlottedPage(page.GetPage(), rid.page_id).DeleteRecord(rid);
}

std::optional<RID> TableHeap::GetFirstRID() const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto id : CollectPageIds()) {
        auto page = pool_.ReadPage(id);
        SlottedPage view(page.GetPage(), id);
        if (const auto rid = view.GetFirstRID()) {
            return rid;
        }
    }
    return std::nullopt;
}

std::optional<RID> TableHeap::GetNextRID(RID current) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto ids = CollectPageIds();
    const auto current_page = std::find(ids.begin(), ids.end(), current.page_id);
    if (current_page == ids.end()) {
        throw std::out_of_range("RID page is not in this table");
    }
    {
        auto page = pool_.ReadPage(current.page_id);
        SlottedPage view(page.GetPage(), current.page_id);
        if (const auto rid = view.GetNextRID(current)) {
            return rid;
        }
    }
    for (auto page_id = std::next(current_page); page_id != ids.end(); ++page_id) {
        auto page = pool_.ReadPage(*page_id);
        if (const auto rid = SlottedPage(page.GetPage(), *page_id).GetFirstRID()) { return rid; }
    }
    return std::nullopt;
}

void TableHeap::DeletePages() {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto ids = CollectPageIds();
    for (const auto id : ids) {
        if (!pool_.CanDeletePage(id)) {
            throw std::runtime_error("Cannot drop a table with pinned pages");
        }
    }
    for (const auto id : ids) {
        if (!pool_.DeletePage(id)) {
            throw std::runtime_error("Page became pinned during table deletion");
        }
    }
    first_page_id_ = -1;
}

}  // namespace udb
