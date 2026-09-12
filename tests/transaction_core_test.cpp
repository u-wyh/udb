#include "udb/database.h"
#include "udb/sql/binder.h"
#include "udb/sql/engine.h"
#include "udb/transaction.h"

#include <chrono>
#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

void TestStateMachine() {
    TransactionManager manager;
    auto& first = manager.Begin();
    auto& second = manager.Begin();
    Check(first.GetId() == 0 && second.GetId() == 1 && first.IsActive() &&
          manager.GetActiveCount() == 2, "Transaction allocation is wrong");
    Check(&manager.GetTransaction(first.GetId()) == &first, "Transaction lookup is wrong");
    manager.Commit(first);
    Check(first.GetState() == TransactionState::Committed && manager.GetActiveCount() == 1,
          "Commit state transition is wrong");
    Reject<std::logic_error>([&] { manager.Commit(first); });
    Reject<std::logic_error>([&] { manager.Abort(first); });
    manager.Abort(second);
    Check(second.GetState() == TransactionState::Aborted && manager.GetActiveCount() == 0,
          "Abort state transition is wrong");

    TransactionManager other;
    auto& foreign = other.Begin();
    Reject<std::invalid_argument>([&] { manager.Commit(foreign); });
    Reject<std::out_of_range>([&] { manager.GetTransaction(999); });
}

void TestExecutionContext(const std::filesystem::path& path) {
    auto database = Database::Create(path, 1);
    SqlEngine sql(database->GetCatalog());
    TransactionManager manager;
    auto& transaction = manager.Begin();
    ExecutionContext context(transaction);
    auto result = sql.ExecuteSQL("CREATE TABLE t (id INTEGER, name VARCHAR(20))", context);
    Check(result.transaction_id == transaction.GetId(), "CREATE lost its transaction context");
    result = sql.ExecuteSQL("INSERT INTO t VALUES (1, 'one')", context);
    Check(result.transaction_id == transaction.GetId() && result.affected_rows == 1,
          "INSERT lost its transaction context");
    result = sql.ExecuteSQL("SELECT name FROM t WHERE id = 1", context);
    Check(result.transaction_id == transaction.GetId() && result.rows.size() == 1 &&
          result.rows[0].GetValue(0) == Value::Varchar("one"),
          "SELECT lost its transaction context");
    Reject<BindError>([&] { sql.ExecuteSQL("SELECT missing FROM t", context); });
    Check(transaction.IsActive(), "Statement error changed transaction state in core stage");
    manager.Commit(transaction);
    Reject<std::logic_error>([&] { sql.ExecuteSQL("SELECT * FROM t", context); });

    auto& aborted = manager.Begin();
    ExecutionContext aborted_context(aborted);
    manager.Abort(aborted);
    Reject<std::logic_error>([&] { sql.ExecuteSQL("SELECT * FROM t", aborted_context); });
    const auto legacy = sql.ExecuteSQL("SELECT * FROM t");
    Check(!legacy.transaction_id && legacy.rows.size() == 1,
          "Context-free SQL compatibility changed");
    database->Close();
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-transaction-core-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestStateMachine();
        TestExecutionContext(directory / "transaction.udb");
        std::cout << "Transaction core tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
