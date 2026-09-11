#pragma once

#include "udb/catalog.h"
#include "udb/sql/ast.h"
#include "udb/sql/bound_statement.h"

namespace udb::sql {

class BindError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Case-sensitive table/column lookup, matching Catalog/Schema conventions.
// Only reads metadata; never accesses TableHeap or executes statements.
class Binder {
public:
    explicit Binder(const Catalog& catalog) : catalog_(catalog) {}
    BoundStatement Bind(const Statement& statement) const;

private:
    BoundCreateTableStatement BindStatement(const CreateTableStatement& statement) const;
    BoundDropTableStatement BindStatement(const DropTableStatement& statement) const;
    BoundInsertStatement BindStatement(const InsertStatement& statement) const;
    BoundSelectStatement BindStatement(const SelectStatement& statement) const;
    BoundDeleteStatement BindStatement(const DeleteStatement& statement) const;
    BoundUpdateStatement BindStatement(const UpdateStatement& statement) const;
    const TableMetadata& Lookup(const std::string& name) const;

    const Catalog& catalog_;
};

}  // namespace udb::sql
