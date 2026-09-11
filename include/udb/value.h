#pragma once

#include "udb/type_id.h"

#include <string>
#include <utility>
#include <variant>

namespace udb {

class Value {
public:
    static Value Boolean(bool value) { return Value(TypeId::BOOLEAN, value); }
    static Value Integer(std::int32_t value) { return Value(TypeId::INTEGER, value); }
    static Value BigInt(std::int64_t value) { return Value(TypeId::BIGINT, value); }
    static Value Varchar(std::string value) { return Value(TypeId::VARCHAR, std::move(value)); }
    static Value Double(double value) { return Value(TypeId::DOUBLE, value); }
    static Value Null(TypeId type) {
        ValidateType(type);
        return Value(type, std::monostate{});
    }

    TypeId GetType() const { return type_; }
    bool IsNull() const { return std::holds_alternative<std::monostate>(data_); }
    bool GetBoolean() const { Require(TypeId::BOOLEAN); return std::get<bool>(data_); }
    std::int32_t GetInteger() const { Require(TypeId::INTEGER); return std::get<std::int32_t>(data_); }
    std::int64_t GetBigInt() const { Require(TypeId::BIGINT); return std::get<std::int64_t>(data_); }
    const std::string& GetVarchar() const { Require(TypeId::VARCHAR); return std::get<std::string>(data_); }
    double GetDouble() const { Require(TypeId::DOUBLE); return std::get<double>(data_); }

    // Structural equality for tests, not SQL three-valued equality.
    // NULLs compare equal only when their declared types also match.
    friend bool operator==(const Value& a, const Value& b) { return a.type_ == b.type_ && a.data_ == b.data_; }
    friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

private:
    using Data = std::variant<std::monostate, bool, std::int32_t, std::int64_t, std::string, double>;
    Value(TypeId type, Data data) : type_(type), data_(std::move(data)) {}
    void Require(TypeId type) const {
        if (type_ != type || IsNull()) {
            throw std::invalid_argument("Value is NULL or has the wrong type");
        }
    }
    TypeId type_;
    Data data_;
};

}  // namespace udb
