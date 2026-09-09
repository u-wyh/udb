#pragma once

#include "udb/type_id.h"

#include <string>
#include <utility>

namespace udb {

class Column {
public:
    // VARCHAR maximum length is in bytes, must be positive. Other types use 0.
    Column(std::string name, TypeId type, std::uint32_t max_length = 0)
        : name_(std::move(name)), type_(type), max_length_(max_length) {
        ValidateType(type);
        if (name_.empty() || (type == TypeId::VARCHAR ? max_length == 0 : max_length != 0)) {
            throw std::invalid_argument("Invalid column name or maximum length");
        }
    }
    const std::string& GetName() const { return name_; }
    TypeId GetType() const { return type_; }
    std::uint32_t GetMaxLength() const { return max_length_; }

private:
    std::string name_;
    TypeId type_;
    std::uint32_t max_length_;
};

}  // namespace udb
