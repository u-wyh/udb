#include "udb/sql/binder.h"

#include <limits>
#include <optional>

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

std::size_t FindColumn(const Schema& schema, const std::string& name) {
    for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) {
        if (schema.GetColumn(i).GetName() == name) { return i; }
    }
    throw BindError("Unknown column: " + name);
}

std::optional<TypeId> InferType(const ExpressionPtr& expression, const Schema& schema) {
    if (!expression) { throw BindError("Missing expression"); }
    if (const auto* column = std::get_if<ColumnExpression>(&expression->node)) {
        return schema.GetColumn(FindColumn(schema, column->name)).GetType();
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&expression->node)) {
        if (std::holds_alternative<std::monostate>(literal->value)) { return std::nullopt; }
        if (const auto* integer = std::get_if<std::int64_t>(&literal->value)) {
            return *integer >= std::numeric_limits<std::int32_t>::min() &&
                   *integer <= std::numeric_limits<std::int32_t>::max() ? TypeId::INTEGER : TypeId::BIGINT;
        }
        if (std::holds_alternative<std::string>(literal->value)) { return TypeId::VARCHAR; }
        return TypeId::BOOLEAN;
    }
    return TypeId::BOOLEAN;
}

Value BindExpressionLiteral(const Literal& literal, std::optional<TypeId> expected) {
    if (std::holds_alternative<std::monostate>(literal)) {
        if (!expected) { throw BindError("Cannot determine NULL type"); }
        return Value::Null(*expected);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&literal)) {
        const bool small = *integer >= std::numeric_limits<std::int32_t>::min() &&
                           *integer <= std::numeric_limits<std::int32_t>::max();
        const auto type = expected.value_or(small ? TypeId::INTEGER : TypeId::BIGINT);
        if (type == TypeId::INTEGER && small) { return Value::Integer(static_cast<std::int32_t>(*integer)); }
        if (type == TypeId::BIGINT) { return Value::BigInt(*integer); }
        throw BindError("Integer literal type mismatch or out of range");
    }
    if (const auto* text = std::get_if<std::string>(&literal)) {
        if (!expected || *expected == TypeId::VARCHAR) { return Value::Varchar(*text); }
        throw BindError("String literal type mismatch");
    }
    if (const auto* boolean = std::get_if<bool>(&literal)) {
        if (!expected || *expected == TypeId::BOOLEAN) { return Value::Boolean(*boolean); }
        throw BindError("Boolean literal type mismatch");
    }
    throw BindError("Unsupported literal");
}

BoundExpressionPtr BindExpression(const ExpressionPtr& expression, const Schema& schema,
                                  std::optional<TypeId> expected = std::nullopt) {
    if (!expression) { throw BindError("Missing expression"); }
    if (const auto* column = std::get_if<ColumnExpression>(&expression->node)) {
        const auto index = FindColumn(schema, column->name);
        const auto type = schema.GetColumn(index).GetType();
        if (expected && *expected != type) { throw BindError("Expression type mismatch"); }
        return std::make_shared<BoundExpression>(type, BoundColumnExpression{index});
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&expression->node)) {
        auto value = BindExpressionLiteral(literal->value, expected);
        return std::make_shared<BoundExpression>(value.GetType(), BoundLiteralExpression{std::move(value)});
    }
    if (const auto* comparison = std::get_if<ComparisonExpression>(&expression->node)) {
        if (expected && *expected != TypeId::BOOLEAN) { throw BindError("Comparison must produce BOOLEAN"); }
        const auto left_hint = InferType(comparison->left, schema);
        const auto right_hint = InferType(comparison->right, schema);
        if (!left_hint && !right_hint) { throw BindError("Cannot compare two untyped NULL values"); }
        const bool left_literal = std::holds_alternative<LiteralExpression>(comparison->left->node);
        const bool right_literal = std::holds_alternative<LiteralExpression>(comparison->right->node);
        std::optional<TypeId> literal_type;
        if (left_literal && right_literal) {
            if (!left_hint) { literal_type = right_hint; }
            else if (!right_hint || *left_hint == *right_hint) { literal_type = left_hint; }
            else if ((*left_hint == TypeId::INTEGER || *left_hint == TypeId::BIGINT) &&
                     (*right_hint == TypeId::INTEGER || *right_hint == TypeId::BIGINT)) {
                literal_type = TypeId::BIGINT;
            } else {
                throw BindError("Comparison operands have incompatible types");
            }
        }
        auto left = BindExpression(comparison->left, schema,
            left_literal ? (right_literal ? literal_type : right_hint) : std::nullopt);
        auto right = BindExpression(comparison->right, schema,
            right_literal ? (left_literal ? literal_type : std::optional<TypeId>(left->type)) : std::nullopt);
        if (left->type != right->type) { throw BindError("Comparison operands have incompatible types"); }
        return std::make_shared<BoundExpression>(TypeId::BOOLEAN,
            BoundComparisonExpression{comparison->op, std::move(left), std::move(right)});
    }
    const auto& logical = std::get<LogicalExpression>(expression->node);
    if (expected && *expected != TypeId::BOOLEAN) { throw BindError("Logical expression must produce BOOLEAN"); }
    auto left = BindExpression(logical.left, schema, TypeId::BOOLEAN);
    BoundExpressionPtr right;
    if (logical.op == LogicalOperator::Not) {
        if (logical.right) { throw BindError("NOT accepts one operand"); }
    } else {
        right = BindExpression(logical.right, schema, TypeId::BOOLEAN);
    }
    return std::make_shared<BoundExpression>(TypeId::BOOLEAN,
        BoundLogicalExpression{logical.op, std::move(left), std::move(right)});
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

BoundCreateIndexStatement Binder::BindStatement(const CreateIndexStatement& statement) const {
    if (statement.index_name.empty()) { throw BindError("CREATE INDEX requires a name"); }
    const auto& table = Lookup(statement.table_name);
    const auto column_index = FindColumn(table.GetSchema(), statement.column_name);
    const auto type = table.GetSchema().GetColumn(column_index).GetType();
    if (type != TypeId::INTEGER && type != TypeId::BIGINT) {
        throw BindError("Index column must be INTEGER or BIGINT");
    }
    for (const auto id : catalog_.ListIndexes()) {
        const auto& metadata = catalog_.GetIndex(id).GetMetadata();
        if (metadata.GetIndexName() == statement.index_name) {
            throw BindError("Index already exists: " + statement.index_name);
        }
        if (metadata.GetTableId() == table.GetTableId() && metadata.GetColumnIndex() == column_index) {
            throw BindError("Table column already has an index");
        }
    }
    return {statement.index_name, table.GetTableId(), column_index};
}

BoundDropTableStatement Binder::BindStatement(const DropTableStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    return {table.GetTableId(), table.GetTableName()};
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
    auto predicate = statement.predicate ? BindExpression(statement.predicate, schema, TypeId::BOOLEAN) : nullptr;
    return {table.GetTableId(), table.GetTableName(), std::move(indexes),
            Schema(std::move(columns)), std::move(predicate)};
}

BoundDeleteStatement Binder::BindStatement(const DeleteStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    const auto& schema = table.GetSchema();
    auto predicate = statement.predicate ? BindExpression(statement.predicate, schema, TypeId::BOOLEAN) : nullptr;
    return {table.GetTableId(), table.GetTableName(), schema, std::move(predicate)};
}

BoundUpdateStatement Binder::BindStatement(const UpdateStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    const auto& schema = table.GetSchema();
    if (statement.assignments.empty()) { throw BindError("UPDATE requires at least one assignment"); }
    std::vector<bool> assigned(schema.GetColumnCount(), false);
    std::vector<BoundUpdateAssignment> assignments;
    assignments.reserve(statement.assignments.size());
    for (const auto& assignment : statement.assignments) {
        const auto index = FindColumn(schema, assignment.column_name);
        if (assigned[index]) { throw BindError("Column assigned more than once: " + assignment.column_name); }
        assigned[index] = true;
        const auto& column = schema.GetColumn(index);
        auto value = BindExpressionLiteral(assignment.value, column.GetType());
        if (!value.IsNull() && column.GetType() == TypeId::VARCHAR &&
            value.GetVarchar().size() > column.GetMaxLength()) {
            throw BindError("VARCHAR too long for column: " + column.GetName());
        }
        assignments.push_back({index, std::move(value)});
    }
    auto predicate = statement.predicate ? BindExpression(statement.predicate, schema, TypeId::BOOLEAN) : nullptr;
    return {table.GetTableId(), table.GetTableName(), schema, std::move(assignments), std::move(predicate)};
}

}  // namespace udb::sql
