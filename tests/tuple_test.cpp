#include "udb/tuple.h"
#include "udb/table_heap.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void ExpectThrow(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

using udb::TypeId;
using udb::Value;
using udb::Column;
using udb::Schema;
using udb::Tuple;

Schema AllTypes() {
    return Schema({Column("flag", TypeId::BOOLEAN), Column("id", TypeId::INTEGER),
                   Column("large", TypeId::BIGINT), Column("text", TypeId::VARCHAR, 2000)});
}

void Equal(const Tuple& a, const Tuple& b, const Schema& schema) {
    for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) {
        Check(a.GetValue(i) == b.GetValue(i), "Tuple values differ");
    }
}

void TestValuesAndSchema() {
    Check(TypeId::INTEGER != TypeId::BIGINT, "Types must be distinct");
    Check(Value::Boolean(true).GetBoolean() && !Value::Boolean(false).GetBoolean(), "BOOLEAN failed");
    Check(Value::Integer(-23).GetInteger() == -23, "INTEGER failed");
    Check(Value::BigInt(5000000000LL).GetBigInt() == 5000000000LL, "BIGINT failed");
    Check(Value::Varchar("abc").GetVarchar() == "abc", "VARCHAR failed");
    Check(Value::Integer(1) != Value::BigInt(1), "Equality must not coerce types");
    for (const auto type : {TypeId::BOOLEAN, TypeId::INTEGER, TypeId::BIGINT, TypeId::VARCHAR}) {
        const auto value = Value::Null(type);
        Check(value.IsNull() && value.GetType() == type && value == Value::Null(type), "Typed NULL failed");
        ExpectThrow<std::invalid_argument>([&] { value.GetBoolean(); });
        ExpectThrow<std::invalid_argument>([&] { value.GetInteger(); });
        ExpectThrow<std::invalid_argument>([&] { value.GetBigInt(); });
        ExpectThrow<std::invalid_argument>([&] { value.GetVarchar(); });
    }
    Check(Value::Null(TypeId::INTEGER) != Value::Null(TypeId::BIGINT), "NULL types must differ");
    ExpectThrow<std::invalid_argument>([] { Value::Integer(1).GetBigInt(); });
    ExpectThrow<std::invalid_argument>([] { Value::BigInt(1).GetInteger(); });
    ExpectThrow<std::invalid_argument>([] { Value::Varchar("1").GetBoolean(); });
    ExpectThrow<std::invalid_argument>([] { Value::Boolean(true).GetVarchar(); });
    ExpectThrow<std::invalid_argument>([] { Value::Null(static_cast<TypeId>(255)); });
    ExpectThrow<std::invalid_argument>([] { Column("x", static_cast<TypeId>(255)); });
    ExpectThrow<std::invalid_argument>([] { Column("", TypeId::INTEGER); });
    ExpectThrow<std::invalid_argument>([] { Column("x", TypeId::VARCHAR); });
    ExpectThrow<std::invalid_argument>([] { Column("x", TypeId::BIGINT, 10); });
    ExpectThrow<std::invalid_argument>([] { Schema({Column("x", TypeId::INTEGER), Column("x", TypeId::BOOLEAN)}); });
    const auto schema = AllTypes();
    Check(schema.GetColumnCount() == 4 && schema.GetColumns().size() == 4, "Schema count failed");
    Check(schema.GetColumn(3).GetName() == "text" && schema.GetColumn(3).GetMaxLength() == 2000 &&
          schema.GetColumn(3).GetType() == TypeId::VARCHAR, "Column metadata failed");
    ExpectThrow<std::out_of_range>([&] { schema.GetColumn(4); });
    const Schema one({Column("i", TypeId::INTEGER)});
    ExpectThrow<std::invalid_argument>([&] { Tuple(one, {}); });
    ExpectThrow<std::invalid_argument>([&] { Tuple(one, {Value::Integer(1), Value::Integer(2)}); });
    ExpectThrow<std::invalid_argument>([&] { Tuple(one, {Value::BigInt(1)}); });
    ExpectThrow<std::invalid_argument>([&] { Tuple(one, {Value::Null(TypeId::VARCHAR)}); });
    const Tuple valid(one, {Value::Integer(1)});
    ExpectThrow<std::out_of_range>([&] { valid.GetValue(1); });
    ExpectThrow<std::invalid_argument>([&] { valid.Serialize(schema); });
    ExpectThrow<std::invalid_argument>([&] { valid.Serialize(Schema({Column("i", TypeId::BIGINT)})); });
    const Schema text({Column("t", TypeId::VARCHAR, 2)});
    ExpectThrow<std::invalid_argument>([&] { Tuple(text, {Value::Varchar("abc")}); });
    const Schema empty({});
    const Tuple no_values(empty, {});
    Check(no_values.Serialize(empty).Size() == 4, "Empty schema encoding failed");
    Equal(no_values, Tuple::Deserialize(no_values.Serialize(empty), empty), empty);
}

void TestEncoding() {
    const auto schema = AllTypes();
    const Tuple tuple(schema, {Value::Boolean(true), Value::Integer(-2),
        Value::BigInt(std::numeric_limits<std::int64_t>::min()), Value::Varchar(std::string("a\0b", 3))});
    const auto record = tuple.Serialize(schema);
    const unsigned char expected[] = {
        4, 0, 0, 0, 0, 1, 0, 254, 255, 255, 255,
        0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 3, 0, 0, 0, 'a', 0, 'b'};
    Check(record.Size() == sizeof(expected) && std::memcmp(record.Data(), expected, sizeof(expected)) == 0,
          "Wire encoding does not match independent little-endian fixture");
    Equal(tuple, Tuple::Deserialize(udb::Record(expected, sizeof(expected)), schema), schema);
    for (const auto integer : {std::numeric_limits<std::int32_t>::min(), -1, 0, std::numeric_limits<std::int32_t>::max()}) {
        for (const auto big : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, std::int64_t{0},
                               std::numeric_limits<std::int64_t>::max()}) {
            const Tuple extremes(schema, {Value::Boolean(false), Value::Integer(integer), Value::BigInt(big), Value::Varchar("")});
            Equal(extremes, Tuple::Deserialize(extremes.Serialize(schema), schema), schema);
        }
    }
    for (unsigned mask = 0; mask < 16; ++mask) {
        std::vector<Value> values{Value::Boolean(false), Value::Integer(10), Value::BigInt(-10), Value::Varchar(u8"你好，UDB")};
        for (std::size_t i = 0; i < 4; ++i) {
            if ((mask & (1U << i)) != 0) {
                values[i] = Value::Null(schema.GetColumn(i).GetType());
            }
        }
        const Tuple mixed(schema, values);
        Equal(mixed, Tuple::Deserialize(mixed.Serialize(schema), schema), schema);
        if (mask == 15) {
            Check(mixed.Serialize(schema).Size() == 8, "NULL payload must be absent");
        }
    }
    const Schema short_text({Column("s", TypeId::VARCHAR, 3)});
    const Tuple utf8(short_text, {Value::Varchar(u8"你")});
    Equal(utf8, Tuple::Deserialize(utf8.Serialize(short_text), short_text), short_text);
    ExpectThrow<std::invalid_argument>([&] { utf8.Serialize(Schema({Column("s", TypeId::VARCHAR, 2)})); });
}

void TestMalformed() {
    const auto schema = AllTypes();
    const auto record = Tuple(schema, {Value::Boolean(true), Value::Integer(42), Value::BigInt(-1),
                                       Value::Varchar("test")}).Serialize(schema);
    // Every strict prefix truncates a real field or payload.
    for (std::size_t size = 0; size < record.Size(); ++size) {
        ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(udb::Record(record.Data(), size), schema); });
    }
    for (const std::size_t position : {0U, 4U, 5U, 6U, 11U, 20U, 21U}) {
        std::string corrupt(record.Data(), record.Size());
        corrupt[position] = static_cast<char>(255);
        ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(udb::Record(corrupt.data(), corrupt.size()), schema); });
    }
    std::string trailing(record.Data(), record.Size());
    trailing.push_back(0);
    ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(udb::Record(trailing.data(), trailing.size()), schema); });
    ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(record, Schema({})); });
    const Schema limited({Column("s", TypeId::VARCHAR, 4)});
    const Schema narrower({Column("s", TypeId::VARCHAR, 3)});
    const auto text = Tuple(limited, {Value::Varchar("four")}).Serialize(limited);
    ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(text, narrower); });
    const unsigned char huge[] = {1, 0, 0, 0, 0, 255, 255, 255, 255};
    const Schema wide({Column("s", TypeId::VARCHAR, std::numeric_limits<std::uint32_t>::max())});
    ExpectThrow<std::runtime_error>([&] { Tuple::Deserialize(udb::Record(huge, sizeof(huge)), wide); });
}

void TestStorageIntegration() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("udb-tuple-" + std::to_string(stamp));
    Check(std::filesystem::create_directory(directory), "Cannot create test directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto path = directory / "database.udb";
    const auto schema = AllTypes();
    std::vector<Tuple> tuples;
    std::vector<udb::RID> rids;
    udb::page_id_t first;
    for (int i = 0; i < 12; ++i) {
        std::string text(1400, static_cast<char>('a' + i));
        text[4] = 0;
        tuples.emplace_back(schema, std::vector<Value>{Value::Boolean(i % 2 == 0), Value::Integer(i),
            Value::Null(TypeId::BIGINT), Value::Varchar(text)});
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap table(pool);
        first = table.GetFirstPageId();
        for (const auto& tuple : tuples) {
            rids.push_back(table.InsertRecord(tuple.Serialize(schema)));
        }
        Check(rids.back().page_id > first, "Integration should span pages");
        for (std::size_t i = 0; i < rids.size(); ++i) {
            Equal(tuples[i], Tuple::Deserialize(table.GetRecord(rids[i]), schema), schema);
        }
        pool.FlushAllPages();
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        udb::TableHeap table(pool, first);
        std::size_t i = 0;
        for (auto rid = table.GetFirstRID(); rid; rid = table.GetNextRID(*rid)) {
            Check(i < rids.size() && *rid == rids[i], "Reopen scan RID mismatch");
            Equal(tuples[i++], Tuple::Deserialize(table.GetRecord(*rid), schema), schema);
        }
        Check(i == tuples.size(), "Reopen lost tuples");
    }
}

}  // namespace

int main() {
    try {
        TestValuesAndSchema();
        TestEncoding();
        TestMalformed();
        TestStorageIntegration();
        std::cout << "Tuple tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
