#include "udb/transaction.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestTupleSiread() {
    TransactionManager manager;
    const RID rid{4, 2};
    auto& reader = manager.Begin(IsolationLevel::Serializable);
    auto& writer = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTupleRead(reader, rid);
    manager.RegisterTupleRead(reader, rid);
    Check(manager.GetTupleSireadCount() == 1 && reader.GetTupleSireads().count(rid) == 1,
          "Tuple SIREAD was not registered idempotently");
    manager.RegisterTupleWrite(writer, rid);
    Check(reader.GetOutgoingRwDependencies().count(writer.GetId()) == 1 &&
              writer.GetIncomingRwDependencies().count(reader.GetId()) == 1,
          "Tuple write did not create reader-to-writer dependency");
    manager.Commit(reader);
    Check(manager.GetTupleSireadCount() == 1,
          "Committed reader SIREAD was discarded too early");
    manager.Commit(writer);

    auto& old_reader = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTupleRead(old_reader, RID{5, 1});
    manager.Commit(old_reader);
    auto& later_writer = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTupleWrite(later_writer, RID{5, 1});
    Check(later_writer.GetIncomingRwDependencies().empty(),
          "Nonconcurrent committed SIREAD created a dependency");
    manager.Abort(later_writer);

    auto& ordinary = manager.Begin(IsolationLevel::SnapshotIsolation);
    bool rejected = false;
    try { manager.RegisterTupleRead(ordinary, rid); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Non-SERIALIZABLE transaction acquired SIREAD");
    manager.Abort(ordinary);
}

}  // namespace

int main() {
    try {
        TestTupleSiread();
        std::cout << "Tuple SIREAD tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
