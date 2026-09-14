#include "udb/transaction.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestSsiCore() {
    TransactionManager manager;
    auto& reader = manager.Begin(IsolationLevel::Serializable);
    auto& writer = manager.Begin(IsolationLevel::Serializable);
    Check(reader.GetIsolationLevel() == IsolationLevel::Serializable &&
              manager.GetRetainedSsiTransactionCount() == 2,
          "SERIALIZABLE transaction state was not created");
    manager.AddRwDependency(reader, writer);
    manager.AddRwDependency(reader, writer);
    Check(reader.GetOutgoingRwDependencies() == std::set<transaction_id_t>{writer.GetId()} &&
              writer.GetIncomingRwDependencies() == std::set<transaction_id_t>{reader.GetId()},
          "SSI rw dependency is missing or duplicated");
    manager.Commit(reader);
    Check(manager.GetRetainedSsiTransactionCount() == 2 &&
              reader.GetOutgoingRwDependencies().count(writer.GetId()) == 1,
          "Committed SSI conflict state was discarded too early");
    manager.Commit(writer);

    auto& ordinary = manager.Begin(IsolationLevel::SnapshotIsolation);
    auto& serializable = manager.Begin(IsolationLevel::Serializable);
    bool rejected = false;
    try { manager.AddRwDependency(ordinary, serializable); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "SSI accepted a non-SERIALIZABLE dependency endpoint");
    manager.Abort(ordinary);
    manager.Abort(serializable);
}

}  // namespace

int main() {
    try {
        TestSsiCore();
        std::cout << "SSI transaction core tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
