#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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

// Schema fixes component types. Signed numbers use sign-flipped big-endian
// bytes; strings escape zero and terminate, preserving component ordering.
inline IndexKey MakeCompositeKey(const std::vector<IndexKey>& components) {
    std::string bytes;
    for (const auto& component : components) {
        if (component.IsString()) {
            for (const char byte : component.GetString()) {
                bytes.push_back(byte);
                if (byte == '\0') { bytes.push_back(static_cast<char>(255)); }
            }
            bytes.append(2, '\0');
        } else {
            const auto value = static_cast<std::uint64_t>(component.GetInteger()) ^ (std::uint64_t{1} << 63);
            for (int shift = 56; shift >= 0; shift -= 8) {
                bytes.push_back(static_cast<char>((value >> shift) & 255));
            }
        }
    }
    return IndexKey(std::move(bytes));
}

}  // namespace udb
