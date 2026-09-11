#include "udb/tuple.h"

#include <cstring>

namespace udb {
namespace {

void Write(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

class Reader {
public:
    explicit Reader(const Record& record) : record_(record) {}
    std::size_t Remaining() const { return record_.Size() - position_; }
    std::uint64_t Read(std::size_t width) {
        Require(width);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(record_.Data()[position_++]))
                     << (8 * i);
        }
        return value;
    }
    std::string ReadString(std::size_t size) {
        Require(size);
        const std::string result(record_.Data() + position_, size);
        position_ += size;
        return result;
    }

private:
    void Require(std::size_t size) const {
        if (size > Remaining()) {
            throw std::runtime_error("Truncated tuple record");
        }
    }
    const Record& record_;
    std::size_t position_ = 0;
};

}  // namespace

Tuple::Tuple(const Schema& schema, std::vector<Value> values) : values_(std::move(values)) {
    Validate(schema);
}

void Tuple::Validate(const Schema& schema) const {
    if (values_.size() != schema.GetColumnCount()) {
        throw std::invalid_argument("Tuple column count mismatch");
    }
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const auto& value = values_[i];
        const auto& column = schema.GetColumn(i);
        if (value.GetType() != column.GetType()) {
            throw std::invalid_argument("Tuple value type mismatch (including typed NULL)");
        }
        if (!value.IsNull() && value.GetType() == TypeId::VARCHAR &&
            value.GetVarchar().size() > column.GetMaxLength()) {
            throw std::invalid_argument("Tuple VARCHAR exceeds column byte limit");
        }
    }
}

Record Tuple::Serialize(const Schema& schema) const {
    Validate(schema);
    std::vector<unsigned char> bytes;
    Write(bytes, values_.size(), 4);
    for (const auto& value : values_) {
        Write(bytes, value.IsNull() ? 1 : 0, 1);
        if (value.IsNull()) {
            continue;
        }
        switch (value.GetType()) {
            case TypeId::BOOLEAN: Write(bytes, value.GetBoolean() ? 1 : 0, 1); break;
            case TypeId::INTEGER: Write(bytes, static_cast<std::uint32_t>(value.GetInteger()), 4); break;
            case TypeId::BIGINT: Write(bytes, static_cast<std::uint64_t>(value.GetBigInt()), 8); break;
            case TypeId::VARCHAR: {
                const auto& text = value.GetVarchar();
                Write(bytes, text.size(), 4);
                bytes.insert(bytes.end(), text.begin(), text.end());
                break;
            }
            case TypeId::DOUBLE: {
                std::uint64_t raw = 0;
                const auto number = value.GetDouble();
                std::memcpy(&raw, &number, sizeof(raw));
                Write(bytes, raw, 8);
                break;
            }
        }
    }
    return Record(bytes.data(), bytes.size());
}

Tuple Tuple::Deserialize(const Record& record, const Schema& schema) {
    Reader reader(record);
    const auto count = reader.Read(4);
    if (count != schema.GetColumnCount() || count > reader.Remaining()) {
        throw std::runtime_error("Invalid tuple column count");
    }
    std::vector<Value> values;
    values.reserve(static_cast<std::size_t>(count));
    for (const auto& column : schema.GetColumns()) {
        const auto is_null = reader.Read(1);
        if (is_null > 1) {
            throw std::runtime_error("Invalid tuple NULL flag");
        }
        if (is_null == 1) {
            values.push_back(Value::Null(column.GetType()));
            continue;
        }
        switch (column.GetType()) {
            case TypeId::BOOLEAN: {
                const auto value = reader.Read(1);
                if (value > 1) {
                    throw std::runtime_error("Invalid tuple BOOLEAN");
                }
                values.push_back(Value::Boolean(value == 1));
                break;
            }
            case TypeId::INTEGER: {
                const auto raw = reader.Read(4);
                const auto value = raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
                    ? static_cast<std::int64_t>(raw) : static_cast<std::int64_t>(raw) - (std::int64_t{1} << 32);
                values.push_back(Value::Integer(static_cast<std::int32_t>(value)));
                break;
            }
            case TypeId::BIGINT: {
                const auto raw = reader.Read(8);
                const auto value = raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                    ? static_cast<std::int64_t>(raw) : -1 - static_cast<std::int64_t>(~raw);
                values.push_back(Value::BigInt(value));
                break;
            }
            case TypeId::VARCHAR: {
                const auto size = reader.Read(4);
                if (size > column.GetMaxLength()) {
                    throw std::runtime_error("Tuple VARCHAR exceeds column byte limit");
                }
                values.push_back(Value::Varchar(reader.ReadString(static_cast<std::size_t>(size))));
                break;
            }
            case TypeId::DOUBLE: {
                const auto raw = reader.Read(8);
                double number = 0;
                std::memcpy(&number, &raw, sizeof(number));
                values.push_back(Value::Double(number));
                break;
            }
        }
    }
    if (reader.Remaining() != 0) {
        throw std::runtime_error("Trailing bytes in tuple record");
    }
    return Tuple(schema, std::move(values));
}

}  // namespace udb
