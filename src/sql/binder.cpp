#include "udb/sql/binder.h"

#include <algorithm>
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
    if (name.find('.') == std::string::npos) {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) {
            const auto& candidate = schema.GetColumn(i).GetName();
            const auto dot = candidate.rfind('.');
            if (dot != std::string::npos && candidate.substr(dot + 1) == name) {
                if (found) { throw BindError("Ambiguous column: " + name); }
                found = i;
            }
        }
        if (found) { return *found; }
    }
    throw BindError("Unknown column: " + name);
}

Schema JoinSchema(const std::string& left_name, const Schema& left,
                  const std::string& right_name, const Schema& right) {
    std::vector<Column> columns;
    columns.reserve(left.GetColumnCount() + right.GetColumnCount());
    for (const auto& column : left.GetColumns()) {
        columns.emplace_back(left_name + "." + column.GetName(), column.GetType(), column.GetMaxLength());
    }
    for (const auto& column : right.GetColumns()) {
        columns.emplace_back(right_name + "." + column.GetName(), column.GetType(), column.GetMaxLength());
    }
    return Schema(std::move(columns));
}

std::string AggregateName(AggregateType type, const std::optional<std::string>& column) {
    const char* name = nullptr;
    switch (type) {
        case AggregateType::Count: name = "count"; break;
        case AggregateType::Sum: name = "sum"; break;
        case AggregateType::Min: name = "min"; break;
        case AggregateType::Max: name = "max"; break;
        case AggregateType::Avg: name = "avg"; break;
    }
    return std::string(name) + "(" + (column ? *column : "*") + ")";
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
    if (const auto* arithmetic = std::get_if<ArithmeticExpression>(&expression->node)) {
        const auto left = InferType(arithmetic->left, schema);
        const auto right = InferType(arithmetic->right, schema);
        if (left && right && *left != *right) {
            if ((*left == TypeId::INTEGER || *left == TypeId::BIGINT) &&
                (*right == TypeId::INTEGER || *right == TypeId::BIGINT)) {
                return TypeId::BIGINT;
            }
            return std::nullopt;
        }
        return left ? left : right;
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
        if (type == TypeId::DOUBLE) { return Value::Double(static_cast<double>(*integer)); }
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
    if (const auto* arithmetic = std::get_if<ArithmeticExpression>(&expression->node)) {
        const auto right_hint = InferType(arithmetic->right, schema);
        const bool left_literal = std::holds_alternative<LiteralExpression>(arithmetic->left->node);
        const bool right_literal = std::holds_alternative<LiteralExpression>(arithmetic->right->node);
        auto left = BindExpression(arithmetic->left, schema,
            left_literal ? right_hint : std::nullopt);
        auto right = BindExpression(arithmetic->right, schema,
            right_literal ? std::optional<TypeId>(left->type) : std::nullopt);
        if (left->type != right->type ||
            (left->type != TypeId::INTEGER && left->type != TypeId::BIGINT)) {
            throw BindError("Arithmetic operands must have the same INTEGER or BIGINT type");
        }
        if (expected && *expected != left->type) { throw BindError("Arithmetic expression type mismatch"); }
        const auto result_type = left->type;
        return std::make_shared<BoundExpression>(result_type,
            BoundArithmeticExpression{arithmetic->op, std::move(left), std::move(right)});
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
    std::vector<std::size_t> columns;
    const auto names = statement.column_names.empty() ? std::vector<std::string>{statement.column_name} : statement.column_names;
    for (const auto& name : names) { columns.push_back(FindColumn(table.GetSchema(), name)); }
    IndexKeyWidth(table.GetSchema(), columns);
    for (const auto id : catalog_.ListIndexes()) {
        const auto& metadata = catalog_.GetIndex(id).GetMetadata();
        if (metadata.GetIndexName() == statement.index_name) {
            throw BindError("Index already exists: " + statement.index_name);
        }
        if (metadata.GetTableId() == table.GetTableId() && metadata.GetColumnIndexes() == columns) {
            throw BindError("Table column already has an index");
        }
    }
    return {statement.index_name, table.GetTableId(), columns.front(), columns};
}

BoundDropTableStatement Binder::BindStatement(const DropTableStatement& statement) const {
    const auto& table = Lookup(statement.table_name);
    return {table.GetTableId(), table.GetTableName()};
}

BoundDropIndexStatement Binder::BindStatement(const DropIndexStatement& statement) const {
    try {
        const auto& metadata = catalog_.GetIndex(statement.index_name).GetMetadata();
        return {metadata.GetIndexId(), metadata.GetIndexName()};
    } catch (const std::out_of_range&) {
        throw BindError("Index not found: " + statement.index_name);
    }
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
    Schema schema = table.GetSchema();
    std::optional<table_id_t> second_table_id;
    std::optional<std::string> second_table_name;
    if (statement.joined_table) {
        const auto& second = Lookup(*statement.joined_table);
        if (second.GetTableId() == table.GetTableId()) {
            throw BindError("JOIN requires two distinct table names");
        }
        schema = JoinSchema(table.GetTableName(), table.GetSchema(),
                            second.GetTableName(), second.GetSchema());
        second_table_id = second.GetTableId();
        second_table_name = second.GetTableName();
        if (!statement.aggregates.empty()) {
            throw BindError("Aggregates over JOIN are not supported yet");
        }
    }
    auto predicate = statement.predicate ? BindExpression(statement.predicate, schema, TypeId::BOOLEAN) : nullptr;
    BoundExpressionPtr join_condition;
    if (statement.join_condition) {
        if (!second_table_id) { throw BindError("ON requires a joined table"); }
        join_condition = BindExpression(statement.join_condition, schema, TypeId::BOOLEAN);
        const auto* comparison = std::get_if<BoundComparisonExpression>(&join_condition->node);
        if (!comparison || comparison->op != ComparisonOperator::Equal) {
            throw BindError("JOIN ON requires column equality");
        }
        const auto* left = std::get_if<BoundColumnExpression>(&comparison->left->node);
        const auto* right = std::get_if<BoundColumnExpression>(&comparison->right->node);
        const auto boundary = table.GetSchema().GetColumnCount();
        if (!left || !right || (left->column_index < boundary) == (right->column_index < boundary)) {
            throw BindError("JOIN ON must compare columns from opposite tables");
        }
    }
    if (!statement.aggregates.empty()) {
        if (statement.select_all) { throw BindError("Cannot combine * with aggregate projections"); }
        std::optional<std::size_t> group_by_column;
        bool project_group_by = false;
        std::vector<Column> output_columns;
        if (statement.group_by) {
            if (statement.column_names.size() > 1 ||
                (!statement.column_names.empty() && statement.column_names[0] != *statement.group_by)) {
                throw BindError("Non-aggregate SELECT columns must match GROUP BY");
            }
            group_by_column = FindColumn(schema, *statement.group_by);
            project_group_by = !statement.column_names.empty();
            if (project_group_by) { output_columns.push_back(schema.GetColumn(*group_by_column)); }
        } else if (!statement.column_names.empty()) {
            throw BindError("Regular columns require GROUP BY in an aggregate query");
        }
        if (!statement.order_by.empty()) {
            throw BindError("ORDER BY aggregates is not supported yet");
        }
        std::vector<BoundAggregate> aggregates;
        for (const auto& aggregate : statement.aggregates) {
            std::optional<std::size_t> column_index;
            TypeId input_type = TypeId::BOOLEAN;
            std::uint32_t max_length = 0;
            if (aggregate.column_name) {
                column_index = FindColumn(schema, *aggregate.column_name);
                const auto& column = schema.GetColumn(*column_index);
                input_type = column.GetType();
                max_length = column.GetMaxLength();
            } else if (aggregate.type != AggregateType::Count) {
                throw BindError("Only COUNT accepts *");
            }
            if ((aggregate.type == AggregateType::Sum || aggregate.type == AggregateType::Avg) &&
                input_type != TypeId::INTEGER && input_type != TypeId::BIGINT) {
                throw BindError("SUM and AVG require INTEGER or BIGINT");
            }
            const auto output_type = aggregate.type == AggregateType::Avg ? TypeId::DOUBLE :
                (aggregate.type == AggregateType::Count || aggregate.type == AggregateType::Sum
                    ? TypeId::BIGINT : input_type);
            output_columns.emplace_back(AggregateName(aggregate.type, aggregate.column_name),
                                        output_type, output_type == TypeId::VARCHAR ? max_length : 0);
            aggregates.push_back({aggregate.type, column_index, input_type});
        }
        Schema output_schema(std::move(output_columns));
        auto having = statement.having
            ? BindExpression(statement.having, output_schema, TypeId::BOOLEAN) : nullptr;
        return {table.GetTableId(), table.GetTableName(), {}, std::move(output_schema),
                std::move(predicate), {}, statement.limit, statement.offset,
                std::move(aggregates), group_by_column, project_group_by, std::move(having), {},
                second_table_id, second_table_name, join_condition};
    }
    if (statement.having) { throw BindError("HAVING requires aggregate projections"); }
    if (statement.group_by) { throw BindError("GROUP BY requires aggregate projections"); }
    if (!statement.projections.empty()) {
        if (statement.select_all || !statement.column_names.empty()) {
            throw BindError("Invalid expression projection state");
        }
        std::vector<BoundExpressionPtr> projections;
        std::vector<Column> columns;
        for (const auto& projection : statement.projections) {
            const bool untyped_null = projection.expression &&
                std::holds_alternative<LiteralExpression>(projection.expression->node) &&
                std::holds_alternative<std::monostate>(
                    std::get<LiteralExpression>(projection.expression->node).value);
            auto expression = BindExpression(projection.expression, schema,
                untyped_null ? std::optional<TypeId>(TypeId::VARCHAR) : std::nullopt);
            std::string name;
            if (projection.alias) {
                if (projection.alias->empty()) { throw BindError("Projection alias cannot be empty"); }
                name = *projection.alias;
            } else if (const auto* column = std::get_if<ColumnExpression>(&projection.expression->node)) {
                name = column->name;
            } else {
                throw BindError("Non-column projection requires AS alias");
            }
            std::uint32_t max_length = 0;
            if (expression->type == TypeId::VARCHAR) {
                if (const auto* column = std::get_if<BoundColumnExpression>(&expression->node)) {
                    max_length = schema.GetColumn(column->column_index).GetMaxLength();
                } else {
                    const auto& literal = std::get<BoundLiteralExpression>(expression->node).value;
                    if (!literal.IsNull()) {
                        const auto& value = literal.GetVarchar();
                        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                            throw BindError("VARCHAR projection is too long");
                        }
                        max_length = static_cast<std::uint32_t>(std::max<std::size_t>(1, value.size()));
                    } else {
                        max_length = 1;
                    }
                }
            }
            columns.emplace_back(std::move(name), expression->type, max_length);
            projections.push_back(std::move(expression));
        }
        std::vector<BoundOrderBy> order_by;
        for (const auto& order : statement.order_by) {
            order_by.push_back({FindColumn(schema, order.column_name), order.ascending});
        }
        return {table.GetTableId(), table.GetTableName(), {}, Schema(std::move(columns)),
                std::move(predicate), std::move(order_by), statement.limit, statement.offset, {},
                std::nullopt, false, nullptr, std::move(projections),
                second_table_id, second_table_name, join_condition};
    }
    std::vector<std::size_t> indexes;
    if (statement.select_all) {
        if (!statement.column_names.empty()) { throw BindError("SELECT cannot combine * with column names"); }
        for (std::size_t i = 0; i < schema.GetColumnCount(); ++i) { indexes.push_back(i); }
    } else {
        if (statement.column_names.empty()) { throw BindError("SELECT requires a projection"); }
        for (const auto& name : statement.column_names) {
            indexes.push_back(FindColumn(schema, name));
        }
    }
    std::vector<Column> columns;
    for (const auto index : indexes) { columns.push_back(schema.GetColumn(index)); }
    // Keep Schema's existing unique-name invariant. Repeated projections are
    // explicitly rejected for now instead of inventing output aliases.
    std::vector<BoundOrderBy> order_by;
    order_by.reserve(statement.order_by.size());
    for (const auto& order : statement.order_by) {
        order_by.push_back({FindColumn(schema, order.column_name), order.ascending});
    }
    return {table.GetTableId(), table.GetTableName(), std::move(indexes),
            Schema(std::move(columns)), std::move(predicate), std::move(order_by),
            statement.limit, statement.offset, {}, std::nullopt, false, nullptr, {},
            second_table_id, second_table_name, join_condition};
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
