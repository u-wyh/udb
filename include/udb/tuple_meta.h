#pragma once

#include <cstdint>

namespace udb {

// Physical metadata associated with a RID. It is deliberately stored outside
// Record/Tuple serialization so logical row bytes remain stable.
struct TupleMeta {
    std::uint64_t timestamp = 0;
    bool is_deleted = false;

    friend bool operator==(const TupleMeta& left, const TupleMeta& right) {
        return left.timestamp == right.timestamp && left.is_deleted == right.is_deleted;
    }
    friend bool operator!=(const TupleMeta& left, const TupleMeta& right) {
        return !(left == right);
    }
};

}  // namespace udb
