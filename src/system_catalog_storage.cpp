#include "udb/system_catalog_storage.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace udb {
namespace {

constexpr std::uint64_t kPageMagic = 0x4547415043544455;  // "UDTCPAGE"
constexpr std::uint32_t kPageVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kPayloadCapacity = PAGE_SIZE - kHeaderSize;

void Put(char* target, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        target[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

std::uint64_t Get(const char* source, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(source[i])) << (8 * i);
    }
    return value;
}

std::uint32_t Checksum(const std::vector<unsigned char>& bytes) {
    std::uint32_t value = 2166136261U;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= 16777619U;
    }
    return value;
}

page_id_t DecodePageId(std::uint64_t value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) { return -1; }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
        throw std::runtime_error("Invalid system catalog page ID");
    }
    return static_cast<page_id_t>(value);
}

void WritePage(Page& page, page_id_t next, const unsigned char* payload,
               std::size_t payload_size, std::size_t total_size, std::uint32_t checksum) {
    page = Page{};
    Put(page.data.data(), kPageMagic, 8);
    Put(page.data.data() + 8, kPageVersion, 4);
    Put(page.data.data() + 12, payload_size, 4);
    Put(page.data.data() + 16,
        next < 0 ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(next), 8);
    Put(page.data.data() + 24, total_size, 4);
    Put(page.data.data() + 28, checksum, 4);
    if (payload_size != 0) {
        std::memcpy(page.data.data() + kHeaderSize, payload, payload_size);
    }
}

struct PageHeader {
    page_id_t next;
    std::uint32_t payload_size;
    std::uint32_t total_size;
    std::uint32_t checksum;
};

PageHeader ReadHeader(const Page& page) {
    if (Get(page.data.data(), 8) != kPageMagic || Get(page.data.data() + 8, 4) != kPageVersion) {
        throw std::runtime_error("Invalid system catalog page header");
    }
    const auto payload_size = Get(page.data.data() + 12, 4);
    const auto total_size = Get(page.data.data() + 24, 4);
    if (payload_size > kPayloadCapacity || total_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Invalid system catalog page size");
    }
    return {DecodePageId(Get(page.data.data() + 16, 8)),
            static_cast<std::uint32_t>(payload_size), static_cast<std::uint32_t>(total_size),
            static_cast<std::uint32_t>(Get(page.data.data() + 28, 4))};
}

}  // namespace

page_id_t SystemCatalogStorage::Create(BufferPoolManager& pool) {
    auto guard = pool.NewPageGuard();
    const auto root = guard.GetPageId();
    const std::vector<unsigned char> empty;
    WritePage(guard.GetPage(), -1, nullptr, 0, 0, Checksum(empty));
    return root;
}

SystemCatalogStorage::SystemCatalogStorage(BufferPoolManager& pool, page_id_t root_page_id)
    : pool_(pool), root_page_id_(root_page_id) {
    if (root_page_id < 0) { throw std::invalid_argument("Invalid system catalog root page ID"); }
}

std::vector<unsigned char> SystemCatalogStorage::Read() const {
    std::vector<unsigned char> result;
    std::set<page_id_t> visited;
    page_id_t current = root_page_id_;
    std::optional<std::uint32_t> expected_total;
    std::optional<std::uint32_t> expected_checksum;
    while (current >= 0) {
        if (!visited.insert(current).second) { throw std::runtime_error("System catalog page chain has a cycle"); }
        const auto guard = pool_.ReadPage(current);
        const auto header = ReadHeader(guard.GetPage());
        if (!expected_total) {
            expected_total = header.total_size;
            expected_checksum = header.checksum;
            result.reserve(*expected_total);
        } else if (header.total_size != *expected_total || header.checksum != *expected_checksum) {
            throw std::runtime_error("Inconsistent system catalog page chain");
        }
        if (result.size() + header.payload_size > *expected_total) {
            throw std::runtime_error("System catalog payload exceeds declared size");
        }
        const auto* begin = reinterpret_cast<const unsigned char*>(guard.GetPage().data.data() + kHeaderSize);
        result.insert(result.end(), begin, begin + header.payload_size);
        current = header.next;
    }
    if (!expected_total || result.size() != *expected_total || Checksum(result) != *expected_checksum) {
        throw std::runtime_error("Invalid system catalog payload");
    }
    return result;
}

void SystemCatalogStorage::Write(const std::vector<unsigned char>& bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("System catalog is too large");
    }
    std::vector<page_id_t> pages;
    std::set<page_id_t> visited;
    page_id_t current = root_page_id_;
    while (current >= 0) {
        if (!visited.insert(current).second) { throw std::runtime_error("System catalog page chain has a cycle"); }
        pages.push_back(current);
        const auto guard = pool_.ReadPage(current);
        current = ReadHeader(guard.GetPage()).next;
    }
    const auto required = std::max<std::size_t>(1, (bytes.size() + kPayloadCapacity - 1) / kPayloadCapacity);
    while (pages.size() < required) {
        auto guard = pool_.NewPageGuard();
        const auto page_id = guard.GetPageId();
        const std::vector<unsigned char> empty;
        WritePage(guard.GetPage(), -1, nullptr, 0, 0, Checksum(empty));
        pages.push_back(page_id);
    }
    const auto checksum = Checksum(bytes);
    for (std::size_t i = 0; i < required; ++i) {
        const auto offset = i * kPayloadCapacity;
        const auto size = std::min(kPayloadCapacity, bytes.size() - std::min(offset, bytes.size()));
        auto guard = pool_.WritePage(pages[i]);
        WritePage(guard.GetPage(), i + 1 < required ? pages[i + 1] : -1,
                  size == 0 ? nullptr : bytes.data() + offset, size, bytes.size(), checksum);
    }
    for (std::size_t i = required; i < pages.size(); ++i) {
        if (!pool_.DeletePage(pages[i])) { throw std::runtime_error("Cannot release system catalog page"); }
    }
}

}  // namespace udb
