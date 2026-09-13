#include "udb/slotted_page.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace udb {
namespace {

// Explicit little-endian fields, independent of struct padding/alignment.
// Both versions use magic:u32, slot_count:u16, data_begin:u16 and next_page:u64.
// v1 slots are offset:u16, size:u16, valid:u16. v2 slots are offset:u16,
// size:u16, state:u8, reserved:u24, timestamp:u64.
constexpr std::uint64_t kLegacyMagic = 0x31504455;
constexpr std::uint64_t kMagic = 0x32504455;
constexpr std::size_t kLegacySlotSize = 6;
constexpr std::uint64_t kOccupied = 1;
constexpr std::uint64_t kDeleted = 2;
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

SlottedPage::SlottedPage(Page& page, page_id_t page_id)
    : page_(page), writable_(&page), page_id_(page_id) {
    if (page_id < 0) {
        throw std::invalid_argument("Slotted page requires a nonnegative page ID");
    }
}

SlottedPage::SlottedPage(const Page& page, page_id_t page_id)
    : page_(page), writable_(nullptr), page_id_(page_id) {
    if (page_id < 0) {
        throw std::invalid_argument("Slotted page requires a nonnegative page ID");
    }
}

Page& SlottedPage::MutablePage() {
    if (writable_ == nullptr) { throw std::logic_error("Cannot modify a read-only slotted page"); }
    return *writable_;
}

void SlottedPage::Init() {
    auto& page = MutablePage();
    page = Page{};
    Write(page, 0, 4, kMagic);
    Write(page, 6, 2, PAGE_SIZE);
    Write(page, 8, 8, kNoNext);
}

std::size_t SlottedPage::SerializedSlotSize() const {
    const auto magic = Read(page_, 0, 4);
    if (magic == kMagic) { return SLOT_SIZE; }
    if (magic == kLegacyMagic) { return kLegacySlotSize; }
    throw std::runtime_error("Invalid slotted page magic");
}

bool SlottedPage::UpgradeLegacyFormat() {
    if (Read(page_, 0, 4) == kMagic) { return true; }
    Validate();
    const auto count = Read(page_, 4, 2);
    const auto begin = Read(page_, 6, 2);
    if (HEADER_SIZE + count * SLOT_SIZE > begin) { return false; }

    Page upgraded = page_;
    std::memset(upgraded.data.data() + HEADER_SIZE, 0,
                static_cast<std::size_t>(count) * SLOT_SIZE);
    Write(upgraded, 0, 4, kMagic);
    for (std::size_t i = 0; i < count; ++i) {
        const auto old_slot = HEADER_SIZE + i * kLegacySlotSize;
        if (Read(page_, old_slot + 4, 2) == 0) { continue; }
        const auto slot = HEADER_SIZE + i * SLOT_SIZE;
        Write(upgraded, slot, 2, Read(page_, old_slot, 2));
        Write(upgraded, slot + 2, 2, Read(page_, old_slot + 2, 2));
        Write(upgraded, slot + 4, 1, kOccupied);
    }
    MutablePage() = upgraded;
    Validate();
    return true;
}

void SlottedPage::Validate() const {
    const auto slot_size = SerializedSlotSize();
    const auto count = Read(page_, 4, 2);
    const auto begin = Read(page_, 6, 2);
    const auto next = Read(page_, 8, 8);
    if (count > (PAGE_SIZE - HEADER_SIZE) / slot_size || begin > PAGE_SIZE ||
        HEADER_SIZE + count * slot_size > begin ||
        (next != kNoNext && next > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max()))) {
        throw std::runtime_error("Invalid slotted page header");
    }
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * slot_size;
        const auto offset = Read(page_, slot, 2);
        const auto size = Read(page_, slot + 2, 2);
        const auto state = Read(page_, slot + 4, slot_size == SLOT_SIZE ? 1 : 2);
        if (state == 0 && offset == 0 && size == 0) {
            continue;
        }
        const bool valid_state = state == kOccupied ||
                                 (slot_size == SLOT_SIZE && state == (kOccupied | kDeleted));
        if (!valid_state || size > end ||
            offset != end - size || offset < begin) {
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
    const auto slot_size = SerializedSlotSize();
    const auto gap = Read(page_, 6, 2) - (HEADER_SIZE + Read(page_, 4, 2) * slot_size);
    return gap >= slot_size ? static_cast<std::size_t>(gap - slot_size) : 0;
}

std::optional<RID> SlottedPage::InsertRecord(const Record& record, TupleMeta meta) {
    Validate();
    if (Read(page_, 0, 4) == kLegacyMagic) {
        const auto count = Read(page_, 4, 2);
        const auto begin = Read(page_, 6, 2);
        const auto directory_end = HEADER_SIZE + count * SLOT_SIZE;
        const bool fits_v2 = directory_end <= begin && begin - directory_end >= SLOT_SIZE &&
                             record.Size() <= begin - directory_end - SLOT_SIZE;
        if (fits_v2) {
            UpgradeLegacyFormat();
        } else if (meta != TupleMeta{}) {
            return std::nullopt;
        }
    }
    const auto slot_size = SerializedSlotSize();
    const auto count = Read(page_, 4, 2);
    const auto begin = Read(page_, 6, 2);
    const auto slot = HEADER_SIZE + static_cast<std::size_t>(count) * slot_size;
    const auto gap = begin - slot;
    if (gap < slot_size || record.Size() > gap - slot_size) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(begin) - record.Size();
    auto& page = MutablePage();
    if (record.Size() != 0) {
        std::memcpy(page.data.data() + offset, record.Data(), record.Size());
    }
    Write(page, slot, 2, offset);
    Write(page, slot + 2, 2, record.Size());
    if (slot_size == SLOT_SIZE) {
        Write(page, slot + 4, 1, kOccupied | (meta.is_deleted ? kDeleted : 0));
        Write(page, slot + 8, 8, meta.timestamp);
    } else {
        Write(page, slot + 4, 2, 1);
    }
    Write(page, 4, 2, count + 1);
    Write(page, 6, 2, offset);
    return RID{page_id_, static_cast<slot_id_t>(count)};
}

std::size_t SlottedPage::FindSlot(RID rid) const {
    Validate();
    if (rid.page_id != page_id_ || rid.slot_id >= Read(page_, 4, 2)) {
        throw std::out_of_range("RID does not belong to this page");
    }
    const auto slot_size = SerializedSlotSize();
    const auto slot = HEADER_SIZE + static_cast<std::size_t>(rid.slot_id) * slot_size;
    if ((Read(page_, slot + 4, slot_size == SLOT_SIZE ? 1 : 2) & kOccupied) == 0) {
        throw std::out_of_range("RID has been deleted");
    }
    return slot;
}

Record SlottedPage::GetRecord(RID rid) const {
    const auto slot = FindSlot(rid);
    return Record(page_.data.data() + Read(page_, slot, 2), Read(page_, slot + 2, 2));
}

TupleMeta SlottedPage::GetTupleMeta(RID rid) const {
    const auto slot = FindSlot(rid);
    if (SerializedSlotSize() == kLegacySlotSize) { return {}; }
    return TupleMeta{Read(page_, slot + 8, 8),
                     (Read(page_, slot + 4, 1) & kDeleted) != 0};
}

void SlottedPage::SetTupleMeta(RID rid, TupleMeta meta) {
    FindSlot(rid);
    if (SerializedSlotSize() == kLegacySlotSize) {
        if (meta == TupleMeta{}) { return; }
        if (!UpgradeLegacyFormat()) {
            throw std::length_error("Legacy page has no room for tuple metadata upgrade");
        }
    }
    const auto slot = FindSlot(rid);
    Write(MutablePage(), slot + 4, 1, kOccupied | (meta.is_deleted ? kDeleted : 0));
    Write(MutablePage(), slot + 8, 8, meta.timestamp);
}

bool SlottedPage::UpdateRecord(RID rid, const Record& record) {
    const auto updated = FindSlot(rid);
    const auto slot_size = SerializedSlotSize();
    const auto count = Read(page_, 4, 2);
    const auto directory_end = HEADER_SIZE + static_cast<std::size_t>(count) * slot_size;
    const auto old_size = static_cast<std::size_t>(Read(page_, updated + 2, 2));
    const auto payload_size = PAGE_SIZE - static_cast<std::size_t>(Read(page_, 6, 2));
    if (record.Size() > old_size + (PAGE_SIZE - directory_end - payload_size)) {
        return false;
    }

    Page compacted;
    std::memcpy(compacted.data.data(), page_.data.data(), directory_end);
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * slot_size;
        if ((Read(page_, slot + 4, slot_size == SLOT_SIZE ? 1 : 2) & kOccupied) == 0) { continue; }
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
    MutablePage() = compacted;
    return true;
}

void SlottedPage::DeleteRecord(RID rid) {
    const auto deleted = FindSlot(rid);
    // Repack into a fixed-size scratch Page to avoid overlapping copies. Keep
    // every slot number, including tombstones, and publish only when complete.
    Page compacted;
    const auto count = Read(page_, 4, 2);
    const auto slot_size = SerializedSlotSize();
    const auto directory_end = HEADER_SIZE + static_cast<std::size_t>(count) * slot_size;
    std::memcpy(compacted.data.data(), page_.data.data(), directory_end);
    std::memset(compacted.data.data() + deleted, 0, slot_size);
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = HEADER_SIZE + i * slot_size;
        if (slot == deleted ||
            (Read(page_, slot + 4, slot_size == SLOT_SIZE ? 1 : 2) & kOccupied) == 0) {
            continue;
        }
        const auto size = static_cast<std::size_t>(Read(page_, slot + 2, 2));
        end -= size;
        std::memcpy(compacted.data.data() + end, page_.data.data() + Read(page_, slot, 2), size);
        Write(compacted, slot, 2, end);
    }
    Write(compacted, 6, 2, end);
    MutablePage() = compacted;
}

std::optional<RID> SlottedPage::GetFirstRID() const {
    Validate();
    const auto slot_size = SerializedSlotSize();
    for (std::size_t i = 0; i < Read(page_, 4, 2); ++i) {
        if ((Read(page_, HEADER_SIZE + i * slot_size + 4,
                  slot_size == SLOT_SIZE ? 1 : 2) & kOccupied) != 0) {
            return RID{page_id_, static_cast<slot_id_t>(i)};
        }
    }
    return std::nullopt;
}

std::optional<RID> SlottedPage::GetNextRID(RID current) const {
    FindSlot(current);
    const auto slot_size = SerializedSlotSize();
    for (std::size_t i = static_cast<std::size_t>(current.slot_id) + 1; i < Read(page_, 4, 2); ++i) {
        if ((Read(page_, HEADER_SIZE + i * slot_size + 4,
                  slot_size == SLOT_SIZE ? 1 : 2) & kOccupied) != 0) {
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
    Write(MutablePage(), 8, 8, page_id == -1 ? kNoNext : static_cast<std::uint64_t>(page_id));
}

}  // namespace udb
