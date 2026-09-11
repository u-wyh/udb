#include "udb/table_heap.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace udb {
namespace {

// Own exactly one pin, including when validation/record allocation throws.
class PinnedPage {
public:
    PinnedPage(BufferPoolManager& pool, page_id_t id)
        : pool_(pool), id_(id), page_(pool.FetchPage(id)) {}
    explicit PinnedPage(BufferPoolManager& pool) : pool_(pool) {
        std::tie(id_, page_) = pool.NewPage();
    }
    ~PinnedPage() { pool_.UnpinPage(id_, dirty_); }
    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    SlottedPage View() { return SlottedPage(*page_, id_); }
    page_id_t Id() const { return id_; }
    void MarkDirty() { dirty_ = true; }

private:
    BufferPoolManager& pool_;
    page_id_t id_ = -1;
    Page* page_ = nullptr;
    bool dirty_ = false;
};

}  // namespace

TableHeap::TableHeap(BufferPoolManager& pool) : pool_(pool), first_page_id_(-1) {
    PinnedPage page(pool_);
    page.View().Init();
    page.MarkDirty();
    first_page_id_ = page.Id();
}

TableHeap::TableHeap(BufferPoolManager& pool, page_id_t first_page_id)
    : pool_(pool), first_page_id_(first_page_id) {
    if (first_page_id < 0) {
        throw std::invalid_argument("Table requires a valid first page ID");
    }
    CollectPageIds();
}

std::vector<page_id_t> TableHeap::CollectPageIds() const {
    std::vector<page_id_t> ids;
    std::unordered_set<page_id_t> seen;
    for (auto id = first_page_id_; id != -1;) {
        if (!seen.insert(id).second) {
            throw std::runtime_error("Table page chain contains a cycle");
        }
        ids.push_back(id);
        PinnedPage page(pool_, id);
        auto view = page.View();
        id = view.GetNextPageId();
    }
    return ids;
}

void TableHeap::RequireMember(page_id_t page_id) const {
    const auto ids = CollectPageIds();
    if (std::find(ids.begin(), ids.end(), page_id) != ids.end()) { return; }
    throw std::out_of_range("RID page is not in this table");
}

RID TableHeap::InsertRecord(const Record& record) {
    if (record.Size() > PAGE_SIZE - SlottedPage::HEADER_SIZE - SlottedPage::SLOT_SIZE) {
        throw std::length_error("Record cannot fit in an empty table page");
    }
    const auto ids = CollectPageIds();
    for (const auto id : ids) {
        PinnedPage page(pool_, id);
        auto view = page.View();
        if (const auto rid = view.InsertRecord(record)) {
            page.MarkDirty();
            return *rid;
        }
    }

    RID inserted;
    {
        PinnedPage page(pool_);
        auto view = page.View();
        view.Init();
        page.MarkDirty();
        inserted = view.InsertRecord(record).value();
    }
    // Release the new page before refetching the tail: capacity one is enough.
    // Publish the link only after the new page has been initialized successfully.
    {
        PinnedPage tail(pool_, ids.back());
        auto view = tail.View();
        if (view.GetNextPageId() != -1) {
            throw std::runtime_error("Invalid table tail during append");
        }
        view.SetNextPageId(inserted.page_id);
        tail.MarkDirty();
    }
    return inserted;
}

Record TableHeap::GetRecord(RID rid) const {
    RequireMember(rid.page_id);
    PinnedPage page(pool_, rid.page_id);
    return page.View().GetRecord(rid);
}

bool TableHeap::UpdateRecord(RID rid, const Record& record) {
    RequireMember(rid.page_id);
    PinnedPage page(pool_, rid.page_id);
    if (!page.View().UpdateRecord(rid, record)) { return false; }
    page.MarkDirty();
    return true;
}

void TableHeap::DeleteRecord(RID rid) {
    RequireMember(rid.page_id);
    PinnedPage page(pool_, rid.page_id);
    page.View().DeleteRecord(rid);
    page.MarkDirty();
}

std::optional<RID> TableHeap::GetFirstRID() const {
    for (const auto id : CollectPageIds()) {
        PinnedPage page(pool_, id);
        auto view = page.View();
        if (const auto rid = view.GetFirstRID()) {
            return rid;
        }
    }
    return std::nullopt;
}

std::optional<RID> TableHeap::GetNextRID(RID current) const {
    const auto ids = CollectPageIds();
    const auto current_page = std::find(ids.begin(), ids.end(), current.page_id);
    if (current_page == ids.end()) {
        throw std::out_of_range("RID page is not in this table");
    }
    {
        PinnedPage page(pool_, current.page_id);
        auto view = page.View();
        if (const auto rid = view.GetNextRID(current)) {
            return rid;
        }
    }
    for (auto page_id = std::next(current_page); page_id != ids.end(); ++page_id) {
        PinnedPage page(pool_, *page_id);
        if (const auto rid = page.View().GetFirstRID()) { return rid; }
    }
    return std::nullopt;
}

void TableHeap::DeletePages() {
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
