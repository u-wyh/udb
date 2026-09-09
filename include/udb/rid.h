#pragma once

#include "udb/page.h"

#include <tuple>

namespace udb {

using slot_id_t = std::uint16_t;

struct RID {
    page_id_t page_id = -1;
    slot_id_t slot_id = 0;

    friend bool operator==(const RID& a, const RID& b) {
        return a.page_id == b.page_id && a.slot_id == b.slot_id;
    }
    friend bool operator!=(const RID& a, const RID& b) { return !(a == b); }
    friend bool operator<(const RID& a, const RID& b) {
        return std::tie(a.page_id, a.slot_id) < std::tie(b.page_id, b.slot_id);
    }
    friend bool operator>(const RID& a, const RID& b) { return b < a; }
    friend bool operator<=(const RID& a, const RID& b) { return !(b < a); }
    friend bool operator>=(const RID& a, const RID& b) { return !(a < b); }
};

}  // namespace udb
