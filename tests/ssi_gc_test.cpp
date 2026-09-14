#include "udb/transaction.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestWatermarkGc() {
    TransactionManager manager;
    auto& reader = manager.Begin(IsolationLevel::Serializable);
    auto& writer = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTupleRead(reader, RID{1, 0});
    manager.RegisterTableRead(reader, 5);
    manager.RegisterTupleWrite(writer, RID{1, 0});
    manager.Commit(reader);
    Check(manager.GetRetainedSsiTransactionCount() == 2 &&
              manager.GarbageCollectSsi() == 0,
          "SSI GC crossed an active transaction watermark");
    manager.Commit(writer);
    Check(manager.GetRetainedSsiTransactionCount() == 0 &&
              manager.GetTupleSireadCount() == 0 &&
              manager.GetPredicateSireadCount() == 0,
          "SSI GC retained metadata after the watermark advanced");

    auto& blocker = manager.Begin(IsolationLevel::SnapshotIsolation);
    auto& completed = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterIndexRead(completed, 9, 3, IndexKey(1), true, IndexKey(8), true);
    manager.Commit(completed);
    Check(manager.GetRetainedSsiTransactionCount() == 1,
          "SSI GC ignored a long-running snapshot");
    manager.Commit(blocker);
    Check(manager.GetRetainedSsiTransactionCount() == 0 &&
              manager.GetPredicateSireadCount() == 0,
          "SSI GC did not reclaim metadata after a long snapshot ended");

    auto& aborted = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTableRead(aborted, 11);
    manager.Abort(aborted);
    Check(manager.GetRetainedSsiTransactionCount() == 0,
          "Aborted Serializable metadata was retained");
}

}  // namespace

int main() {
    try {
        TestWatermarkGc();
        std::cout << "SSI garbage collection tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
