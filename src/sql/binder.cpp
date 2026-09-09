#include "udb/sql/binder.h"

#include <limits>

namespace udb::sql {
namespace {

Value BindLiteral(const Literal& literal, const Column& column) {
    if (std::holds_alternative<std::monostate>(literal)) { return Value::Null(column.GetType()); }
    if (const auto* integer = std::get_if<std::int64_t>(&literal)) {
        // AST integer literals are untyped int64 payloads. Assign INTEGER when
        // representable in int32, otherwise BIGINT; do not widen/narrow afterwards.
        const bool small = *integer >= std::numeric_limits<std::int32_t>::min() &&
                           *integer <= std::numeric_limits<std::int32_t>::max();
        if (small && column.GetType() == TypeId::INTEGER) { return Value::Integer(static_cast<std::int32_t>(*integer)); }
        if (!small && column.GetType() == TypeId::BIGINT) { return Value::BigInt(*integer); }
    } else if (const auto* text = std::get_if<std::string>(&literal)) {
        if (column.GetType() == TypeId::VARCHAR) {
            if (text->size() > column.GetMaxLength()) { throw BindError("VARCHAR too long for column: " + column.GetName()); }
            return Value::Varchar(*text);
        }
    } else if (const auto* boolean = std::get_if<bool>(&literal)) {
        if (column.GetType() == TypeId::BOOLEAN) { return Value::Boolean(*boolean); }
    }
    throw BindError("Literal type does not match column: " + column.GetName());
}

}  // namespace

BoundStatement Binder::Bind(const Statement& statement) const {
    try {
        return std::visit([this](const auto& node) -> BoundStatement { return BindStatement(node); }, statement);
    } catch (const std::invalid_argument& error) {
        throw BindError(error.what());
    }
}

const TableMetadata& Binder::Lookup(const std::string& name) const {
    try { return catalog_.GetTable(name); }
    catch (const std::out_of_range&) { throw BindError("Unknown table: " + name); }
}

BoundCreateTableStatement Binder::BindStatement(const CreateTableStatement& statement) const {
    if (statement.table_name.empty() || statement.columns.empty()) {
        throw BindError("CREATE TABLE requires a name and at least one column");
    }
    for (const auto id : catalog_.ListTables()) {
        if (catalog_.GetTable(id).GetTableName() == statement.table_name) {
            throw BindError("Table already exists: " + statement.table_name);
        }
    }
    std::vector<Column> columns;
    for (const auto& column : statement.columns) {
        columns.emplace_back(column.name, column.type, column.max_length);
    }
    return {statement.table_name, Schema(std::move(columns))};
}

BoundInsertStatement Binder::BindStatement(const InsertStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    const auto& schema = table.GetSchema();
    if (statement.values.size() != schema.GetColumnCount()) { throw BindError("INSERT column count mismatch"); }
    std::vector<Value> values;
    values.reserve(statement.values.size());
    for (std::size_t i = 0; i < statement.values.size(); ++i) {
        values.push_back(BindLiteral(statement.values[i], schema.GetColumn(i)));
    }
    return {table.GetTableId(), table.GetTableName(), schema, std::move(values)};
}

BoundSelectStatement Binder::BindStatement(const SelectStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    const auto& schema = table.GetSchema();
    std::vector<std::size_t> indexes;
    if (statement.select_all) {
        if (!statement.column_names.empty()) { throw BindError("SELECT cannot combine * with column names"); }
        for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) { indexes.push_back(i); }
    } else {
        if (statement.column_names.empty()) { throw BindError("SELECT requires a projection"); }
        for (const auto& name : statement.column_names) {
            std::size_t index = 0;
            while (index < schema.GetColumnCount() && schema.GetColumn(index).GetName() != name) { ++index; }
            if (index == schema.GetColumnCount()) { throw BindError("Unknown column: " + name); }
            indexes.push_back(index);
        }
    }
    std::vector<Column> columns;
    for (const auto index : indexes) { columns.push_back(schema.GetColumn(index)); }
    // Keep Schema's existing unique-name invariant. Repeated projections are
    // explicitly rejected for now instead of inventing output aliases.
    return {table.GetTableId(), table.GetTableName(), std::move(indexes), Schema(std::move(columns))};
}

}  // namespace udb::sql
