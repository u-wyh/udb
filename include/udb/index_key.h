#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace udb {

// Full binary string comparison, including embedded zero bytes. Numeric and
// string keys are distinct; one tree uses only one kind.
class IndexKey {
public:
    IndexKey(std::int64_t value = 0) : value_(value) {}
    IndexKey(std::string value) : value_(std::move(value)) {}
    bool IsString() const { return std::holds_alternative<std::string>(value_); }
    std::int64_t GetInteger() const { return std::get<std::int64_t>(value_); }
    const std::string& GetString() const { return std::get<std::string>(value_); }
    friend bool operator==(const IndexKey& a, const IndexKey& b) { return a.value_ == b.value_; }
    friend bool operator!=(const IndexKey& a, const IndexKey& b) { return !(a == b); }
    friend bool operator<(const IndexKey& a, const IndexKey& b) { return a.value_ < b.value_; }
    friend bool operator>(const IndexKey& a, const IndexKey& b) { return b < a; }
    friend bool operator<=(const IndexKey& a, const IndexKey& b) { return !(b < a); }
    friend bool operator>=(const IndexKey& a, const IndexKey& b) { return !(a < b); }
private:
    std::variant<std::int64_t, std::string> value_;
};

}  // namespace udb
