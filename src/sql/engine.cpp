#include "udb/sql/engine.h"

#include "udb/sql/binder.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

namespace udb::sql {

ExecutionResult SqlEngine::ExecuteSQL(std::string_view sql) {
    const auto statement = Parser::Parse(sql);
    const auto bound = Binder(catalog_).Bind(statement);
    const auto plan = Planner::Plan(bound, catalog_);
    return executor_.Execute(*plan);
}

ExecutionResult SqlEngine::ExecuteSQL(std::string_view sql, ExecutionContext& context) {
    const auto statement = Parser::Parse(sql);
    const auto bound = Binder(catalog_).Bind(statement);
    const auto plan = Planner::Plan(bound, catalog_);
    return executor_.Execute(*plan, context);
}

}  // namespace udb::sql
