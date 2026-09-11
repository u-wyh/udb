#pragma once

#include <cstdint>
#include <stdexcept>

namespace udb {

enum class TypeId : std::uint8_t { BOOLEAN, INTEGER, BIGINT, VARCHAR, DOUBLE };

inline void ValidateType(TypeId type) {
    switch (type) {
        case TypeId::BOOLEAN:
        case TypeId::INTEGER:
        case TypeId::BIGINT:
        case TypeId::VARCHAR:
        case TypeId::DOUBLE:
            return;
    }
    throw std::invalid_argument("Unknown TypeId");
}

}  // namespace udb
