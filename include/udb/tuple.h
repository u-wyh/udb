#pragma once

#include "udb/record.h"
#include "udb/schema.h"
#include "udb/value.h"

namespace udb {

class Tuple {
public:
    Tuple(const Schema& schema, std::vector<Value> values);
    const Value& GetValue(std::size_t index) const { return values_.at(index); }

    // u32 column count; then u8 NULL flag per field (1=NULL, no payload).
    // Non-NULL: bool u8 (0/1), integer 4 bytes, bigint 8 bytes, varchar u32
    // byte length + bytes. Integers use little-endian two's-complement encoding.
    // Schema is external and must be supplied consistently when reading.
    Record Serialize(const Schema& schema) const;
    static Tuple Deserialize(const Record& record, const Schema& schema);

private:
    void Validate(const Schema& schema) const;
    std::vector<Value> values_;
};

}  // namespace udb
