#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace udb {

inline constexpr std::size_t PAGE_SIZE = 4096;
using page_id_t = std::int64_t;

struct Page {
    std::array<char, PAGE_SIZE> data{};
};

}  // namespace udb
