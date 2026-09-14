#include "udb/transaction.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestDangerousPivotIsRejected() {
    TransactionManager manager;
    auto& first = manager.Begin(IsolationLevel::Serializable);
    auto& pivot = manager.Begin(IsolationLevel::Serializable);
    auto& last = manager.Begin(IsolationLevel::Serializable);
    manager.AddRwDependency(first, pivot);
    manager.AddRwDependency(pivot, last);
    Check(manager.HasDangerousStructure(pivot), "SSI pivot was not detected");

    bool rejected = false;
    try {
        manager.Commit(pivot);
    } catch (const SerializationFailure&) {
        rejected = true;
    }
    Check(rejected && pivot.IsActive(), "Dangerous Serializable commit was not rejected safely");
    manager.Abort(pivot);
    manager.Commit(first);
    manager.Commit(last);
}

void TestNonPivotCanCommit() {
    TransactionManager manager;
    auto& reader = manager.Begin(IsolationLevel::Serializable);
    auto& writer = manager.Begin(IsolationLevel::Serializable);
    manager.AddRwDependency(reader, writer);
    Check(!manager.HasDangerousStructure(reader) && !manager.HasDangerousStructure(writer),
          "Single rw edge was treated as a dangerous structure");
    manager.Commit(reader);
    manager.Commit(writer);

    auto& repeatable = manager.Begin(IsolationLevel::RepeatableRead);
    manager.Commit(repeatable);
}

}  // namespace

int main() {
    try {
        TestDangerousPivotIsRejected();
        TestNonPivotCanCommit();
        std::cout << "SSI dangerous structure tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
