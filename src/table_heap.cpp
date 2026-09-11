#include "udb/table_heap.h"

#include <stdexcept>
#include <tuple>

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

page_id_t Next(SlottedPage& page, page_id_t current) {
    const auto next = page.GetNextPageId();
    // DiskManager allocates monotonically increasing IDs. Tail-only appends
    // therefore give a strictly increasing chain, which also rules out cycles.
    if (next != -1 && next <= current) {
        throw std::runtime_error("Table page chain is not strictly increasing");
    }
    return next;
}

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
    for (auto id = first_page_id_; id != -1;) {
        PinnedPage page(pool_, id);
        auto view = page.View();
        id = Next(view, id);
    }
}

void TableHeap::RequireMember(page_id_t page_id) const {
    for (auto id = first_page_id_; id != -1 && id <= page_id;) {
        PinnedPage page(pool_, id);
        auto view = page.View();
        const auto next = Next(view, id);
        if (id == page_id) {
            return;
        }
        id = next;
    }
    throw std::out_of_range("RID page is not in this table");
}

RID TableHeap::InsertRecord(const Record& record) {
    if (record.Size() > PAGE_SIZE - SlottedPage::HEADER_SIZE - SlottedPage::SLOT_SIZE) {
        throw std::length_error("Record cannot fit in an empty table page");
    }
    auto id = first_page_id_;
    while (true) {
        page_id_t next;
        {
            PinnedPage page(pool_, id);
            auto view = page.View();
            next = Next(view, id);
            if (const auto rid = view.InsertRecord(record)) {
                page.MarkDirty();
                return *rid;
            }
        }
        if (next == -1) {
            break;
        }
        id = next;
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
        PinnedPage tail(pool_, id);
        auto view = tail.View();
        if (Next(view, id) != -1 || inserted.page_id <= id) {
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

std::optional<RID> TableHeap::ScanFrom(page_id_t page_id) const {
    for (auto id = page_id; id != -1;) {
        PinnedPage page(pool_, id);
        auto view = page.View();
        const auto next = Next(view, id);
        if (const auto rid = view.GetFirstRID()) {
            return rid;
        }
        id = next;
    }
    return std::nullopt;
}

std::optional<RID> TableHeap::GetFirstRID() const { return ScanFrom(first_page_id_); }

std::optional<RID> TableHeap::GetNextRID(RID current) const {
    RequireMember(current.page_id);
    page_id_t next;
    {
        PinnedPage page(pool_, current.page_id);
        auto view = page.View();
        next = Next(view, current.page_id);
        if (const auto rid = view.GetNextRID(current)) {
            return rid;
        }
    }
    return ScanFrom(next);
}

}  // namespace udb
