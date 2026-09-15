#pragma once

#include "udb/type_id.h"
#include "udb/value.h"

#include <optional>
#include <string>
#include <utility>

namespace udb {

class Column {
public:
    // VARCHAR maximum length is in bytes, must be positive. Other types use 0.
    Column(std::string name, TypeId type, std::uint32_t max_length = 0,
           bool not_null = false, std::optional<Value> default_value = std::nullopt,
           bool primary_key = false, bool unique = false)
        : name_(std::move(name)), type_(type), max_length_(max_length),
          not_null_(not_null || primary_key), default_value_(std::move(default_value)),
          primary_key_(primary_key), unique_(unique || primary_key) {
        ValidateType(type);
        if (name_.empty() || (type == TypeId::VARCHAR ? max_length == 0 : max_length != 0)) {
            throw std::invalid_argument("Invalid column name or maximum length");
        }
        if (default_value_) {
            if (default_value_->GetType() != type_ || (not_null_ && default_value_->IsNull())) {
                throw std::invalid_argument("Column default does not satisfy its type or NOT NULL constraint");
            }
            if (!default_value_->IsNull() && type_ == TypeId::VARCHAR &&
                default_value_->GetVarchar().size() > max_length_) {
                throw std::invalid_argument("Column default exceeds VARCHAR byte limit");
            }
        }
    }
    const std::string& GetName() const { return name_; }
    TypeId GetType() const { return type_; }
    std::uint32_t GetMaxLength() const { return max_length_; }
    bool IsNotNull() const { return not_null_; }
    const std::optional<Value>& GetDefaultValue() const { return default_value_; }
    bool IsPrimaryKey() const { return primary_key_; }
    bool IsUnique() const { return unique_; }

private:
    std::string name_;
    TypeId type_;
    std::uint32_t max_length_;
    bool not_null_;
    std::optional<Value> default_value_;
    bool primary_key_;
    bool unique_;
};

}  // namespace udb
