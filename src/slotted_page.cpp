#include "udb/slotted_page.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace udb {
namespace {

// Explicit little-endian fields, independent of struct padding/alignment:
// magic:u32, slot_count:u16, data_begin:u16, next_page:u64 (-1 = all ones).
// Each slot is offset:u16, size:u16, valid:u16 (0 or 1).
constexpr std::uint64_t kMagic = 0x31504455;
constexpr auto kNoNext = std::numeric_limits<std::uint64_t>::max();

std::uint64_t Read(const Page& page, std::size_t offset, std::size_t width) {
    if (width > 8 || offset > PAGE_SIZE || width > PAGE_SIZE - offset) {
        throw std::runtime_error("Slotted page field is out of bounds");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(page.data[offset + i]))
                 << (8 * i);
    }
    return value;
}

void Write(Page& page, std::size_t offset, std::size_t width, std::uint64_t value) {
    if (width > 8 || offset > PAGE_SIZE || width > PAGE_SIZE - offset) {
        throw std::runtime_error("Slotted page field is out of bounds");
    }
    for (std::size_t i = 0; i < width; ++i) {
        const auto byte = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
        std::memcpy(page.data.data() + offset + i, &byte, 1);
    }
}

}  // namespace

SlottedPage::SlottedPage(Page& page, page_id_t page_id) : page_(page), page_id_(page_id) {
    if (page_id < 0) {
        throw std::invalid_argument("Slotted page requires a nonnegative page ID");
    }
}

void SlottedPage::Init() {
    page_ = Page{};
    Write(page_, 0, 4, kMagic);
    Write(page_, 6, 2, PAGE_SIZE);
    Write(page_, 8, 8, kNoNext);
}

void SlottedPage::Validate() const {
    const auto count = Read(page_, 4, 2);
    const auto begin = Read(page_, 6, 2);
    const auto next = Read(page_, 8, 8);
    if (Read(page_, 0, 4) != kMagic || count > (PAGE_SIZE - HEADER_SIZE) / SLOT_SIZE ||
        begin > PAGE_SIZE || HEADER_SIZE + count * SLOT_SIZE > begin ||
        (next != kNoNext && next > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max()))) {
        throw std::runtime_error("Invalid slotted page header");
    }
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * SLOT_SIZE;
        const auto offset = Read(page_, slot, 2);
        const auto size = Read(page_, slot + 2, 2);
        const auto valid = Read(page_, slot + 4, 2);
        if (valid == 0 && offset == 0 && size == 0) {
            continue;
        }
        if (valid != 1 || size > end || offset != end - size || offset < begin) {
            throw std::runtime_error("Invalid slotted page record bounds");
        }
        end -= static_cast<std::size_t>(size);
    }
    if (end != begin) {
        throw std::runtime_error("Invalid slotted page data boundary");
    }
}

std::size_t SlottedPage::GetFreeSpace() const {
    Validate();
    const auto gap = Read(page_, 6, 2) - (HEADER_SIZE + Read(page_, 4, 2) * SLOT_SIZE);
    return gap >= SLOT_SIZE ? static_cast<std::size_t>(gap - SLOT_SIZE) : 0;
}

std::optional<RID> SlottedPage::InsertRecord(const Record& record) {
    Validate();
    const auto count = Read(page_, 4, 2);
    const auto begin = Read(page_, 6, 2);
    const auto slot = HEADER_SIZE + static_cast<std::size_t>(count) * SLOT_SIZE;
    const auto gap = begin - slot;
    if (gap < SLOT_SIZE || record.Size() > gap - SLOT_SIZE) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(begin) - record.Size();
    if (record.Size() != 0) {
        std::memcpy(page_.data.data() + offset, record.Data(), record.Size());
    }
    Write(page_, slot, 2, offset);
    Write(page_, slot + 2, 2, record.Size());
    Write(page_, slot + 4, 2, 1);
    Write(page_, 4, 2, count + 1);
    Write(page_, 6, 2, offset);
    return RID{page_id_, static_cast<slot_id_t>(count)};
}

std::size_t SlottedPage::FindSlot(RID rid) const {
    Validate();
    if (rid.page_id != page_id_ || rid.slot_id >= Read(page_, 4, 2)) {
        throw std::out_of_range("RID does not belong to this page");
    }
    const auto slot = HEADER_SIZE + static_cast<std::size_t>(rid.slot_id) * SLOT_SIZE;
    if (Read(page_, slot + 4, 2) != 1) {
        throw std::out_of_range("RID has been deleted");
    }
    return slot;
}

Record SlottedPage::GetRecord(RID rid) const {
    const auto slot = FindSlot(rid);
    return Record(page_.data.data() + Read(page_, slot, 2), Read(page_, slot + 2, 2));
}

bool SlottedPage::UpdateRecord(RID rid, const Record& record) {
    const auto updated = FindSlot(rid);
    const auto count = Read(page_, 4, 2);
    const auto directory_end = HEADER_SIZE + static_cast<std::size_t>(count) * SLOT_SIZE;
    const auto old_size = static_cast<std::size_t>(Read(page_, updated + 2, 2));
    const auto payload_size = PAGE_SIZE - static_cast<std::size_t>(Read(page_, 6, 2));
    if (record.Size() > old_size + (PAGE_SIZE - directory_end - payload_size)) {
        return false;
    }

    Page compacted;
    std::memcpy(compacted.data.data(), page_.data.data(), directory_end);
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * SLOT_SIZE;
        if (Read(page_, slot + 4, 2) == 0) { continue; }
        const bool replacement = slot == updated;
        const auto size = replacement ? record.Size() : static_cast<std::size_t>(Read(page_, slot + 2, 2));
        end -= size;
        if (size != 0) {
            const auto* data = replacement ? record.Data() : page_.data.data() + Read(page_, slot, 2);
            std::memcpy(compacted.data.data() + end, data, size);
        }
        Write(compacted, slot, 2, end);
        Write(compacted, slot + 2, 2, size);
    }
    Write(compacted, 6, 2, end);
    page_ = compacted;
    return true;
}

void SlottedPage::DeleteRecord(RID rid) {
    const auto deleted = FindSlot(rid);
    // Repack into a fixed-size scratch Page to avoid overlapping copies. Keep
    // every slot number, including tombstones, and publish only when complete.
    Page compacted;
    const auto count = Read(page_, 4, 2);
    const auto directory_end = HEADER_SIZE + static_cast<std::size_t>(count) * SLOT_SIZE;
    std::memcpy(compacted.data.data(), page_.data.data(), directory_end);
    std::memset(compacted.data.data() + deleted, 0, SLOT_SIZE);
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * SLOT_SIZE;
        if (slot == deleted || Read(page_, slot + 4, 2) == 0) {
            continue;
        }
        const auto size = static_cast<std::size_t>(Read(page_, slot + 2, 2));
        end -= size;
        std::memcpy(compacted.data.data() + end, page_.data.data() + Read(page_, slot, 2), size);
        Write(compacted, slot, 2, end);
    }
    Write(compacted, 6, 2, end);
    page_ = compacted;
}

std::optional<RID> SlottedPage::GetFirstRID() const {
    Validate();
    for (std::size_t i = 0; i < Read(page_, 4, 2); ++i) {
        if (Read(page_, HEADER_SIZE + i * SLOT_SIZE + 4, 2) == 1) {
            return RID{page_id_, static_cast<slot_id_t>(i)};
        }
    }
    return std::nullopt;
}

std::optional<RID> SlottedPage::GetNextRID(RID current) const {
    FindSlot(current);
    for (std::size_t i = static_cast<std::size_t>(current.slot_id) + 1; i < Read(page_, 4, 2); ++i) {
        if (Read(page_, HEADER_SIZE + i * SLOT_SIZE + 4, 2) == 1) {
            return RID{page_id_, static_cast<slot_id_t>(i)};
        }
    }
    return std::nullopt;
}

page_id_t SlottedPage::GetNextPageId() const {
    Validate();
    const auto next = Read(page_, 8, 8);
    return next == kNoNext ? -1 : static_cast<page_id_t>(next);
}

void SlottedPage::SetNextPageId(page_id_t page_id) {
    Validate();
    if (page_id < -1) {
        throw std::invalid_argument("Invalid next page ID");
    }
    Write(page_, 8, 8, page_id == -1 ? kNoNext : static_cast<std::uint64_t>(page_id));
}

}  // namespace udb
