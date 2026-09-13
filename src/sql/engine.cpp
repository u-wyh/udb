#include "udb/sql/engine.h"

#include "udb/sql/binder.h"
#include "udb/sql/parser.h"
#include "udb/sql/planner.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <variant>

namespace udb::sql {

namespace {

std::string NormalizeCommand(std::string_view sql) {
    std::size_t begin = 0;
    std::size_t end = sql.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(sql[begin]))) { ++begin; }
    while (end > begin && std::isspace(static_cast<unsigned char>(sql[end - 1]))) { --end; }
    if (end > begin && sql[end - 1] == ';') {
        --end;
        while (end > begin && std::isspace(static_cast<unsigned char>(sql[end - 1]))) { --end; }
    }
    std::string command(sql.substr(begin, end - begin));
    for (auto& character : command) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return command;
}

bool IsDdl(const Statement& statement) {
    return std::holds_alternative<CreateTableStatement>(statement) ||
           std::holds_alternative<CreateIndexStatement>(statement) ||
           std::holds_alternative<DropTableStatement>(statement) ||
           std::holds_alternative<DropIndexStatement>(statement);
}

void ReloadIndexRoots(Catalog& catalog) {
    for (const auto index_id : catalog.ListIndexes()) {
        catalog.GetIndex(index_id).GetTree().ReloadRootFromHeader();
    }
}

}  // namespace

ExecutionResult SqlEngine::ExecuteSQL(std::string_view sql) {
    const auto command = NormalizeCommand(sql);
    if (command == "BEGIN" || command == "BEGIN TRANSACTION" ||
        command == "BEGIN ISOLATION LEVEL READ COMMITTED" ||
        command == "BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED" ||
        command == "BEGIN ISOLATION LEVEL REPEATABLE READ" ||
        command == "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ" ||
        command == "BEGIN ISOLATION LEVEL SNAPSHOT" ||
        command == "BEGIN TRANSACTION ISOLATION LEVEL SNAPSHOT") {
        if (current_transaction_ != nullptr) {
            throw std::logic_error("A transaction is already active");
        }
        auto isolation_level = default_isolation_;
        if (command.find("READ COMMITTED") != std::string::npos) {
            isolation_level = IsolationLevel::ReadCommitted;
        } else if (command.find("REPEATABLE READ") != std::string::npos) {
            isolation_level = IsolationLevel::RepeatableRead;
        } else if (command.find("SNAPSHOT") != std::string::npos) {
            isolation_level = IsolationLevel::SnapshotIsolation;
        }
        current_transaction_ = &transaction_manager_.Begin(isolation_level);
        ExecutionResult result{PlanType::Begin};
        result.transaction_id = current_transaction_->GetId();
        return result;
    }
    if (command == "COMMIT") {
        if (current_transaction_ == nullptr) { throw std::logic_error("No active transaction"); }
        const auto id = current_transaction_->GetId();
        transaction_manager_.Commit(*current_transaction_);
        current_transaction_ = nullptr;
        ExecutionResult result{PlanType::Commit};
        result.transaction_id = id;
        return result;
    }
    if (command == "ROLLBACK") {
        if (current_transaction_ == nullptr) { throw std::logic_error("No active transaction"); }
        const auto id = current_transaction_->GetId();
        transaction_manager_.Abort(*current_transaction_, [&] { ReloadIndexRoots(catalog_); });
        current_transaction_ = nullptr;
        ExecutionResult result{PlanType::Rollback};
        result.transaction_id = id;
        return result;
    }

    const auto statement = Parser::Parse(sql);
    if (current_transaction_ != nullptr && IsDdl(statement)) {
        throw std::invalid_argument("DDL is not supported inside an explicit transaction");
    }
    const auto bound = Binder(catalog_).Bind(statement);
    const auto plan = Planner::Plan(bound, catalog_);
    const bool autocommit = current_transaction_ == nullptr;
    auto* transaction = autocommit ? &transaction_manager_.Begin(default_isolation_)
                                   : current_transaction_;
    ExecutionContext context(*transaction, catalog_.GetLockManager(), transaction_manager_);
    try {
        auto result = executor_.Execute(*plan, context);
        if (autocommit) {
            transaction_manager_.Commit(*transaction);
            result.transaction_id.reset();
        }
        return result;
    } catch (...) {
        transaction_manager_.Abort(*transaction, [&] { ReloadIndexRoots(catalog_); });
        if (!autocommit) { current_transaction_ = nullptr; }
        throw;
    }
}

void SqlEngine::SetDefaultIsolationLevel(IsolationLevel isolation_level) {
    if (current_transaction_ != nullptr) {
        throw std::logic_error("Cannot change isolation level during a transaction");
    }
    default_isolation_ = isolation_level;
}

ExecutionResult SqlEngine::ExecuteSQL(std::string_view sql, ExecutionContext& context) {
    const auto statement = Parser::Parse(sql);
    const auto bound = Binder(catalog_).Bind(statement);
    const auto plan = Planner::Plan(bound, catalog_);
    return executor_.Execute(*plan, context);
}

}  // namespace udb::sql
