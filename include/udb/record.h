#pragma once

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace udb {

// Owns arbitrary bytes, including embedded zeroes and empty records.
class Record {
public:
    Record() = default;
    Record(const void* data, std::size_t size) {
        if (size != 0) {
            if (data == nullptr) {
                throw std::invalid_argument("Nonempty record requires data");
            }
            data_.resize(size);
            std::memcpy(data_.data(), data, size);
        }
    }

    std::size_t Size() const { return data_.size(); }
    const char* Data() const { return data_.data(); }

private:
    std::vector<char> data_;
};

}  // namespace udb
