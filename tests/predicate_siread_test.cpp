#include "udb/transaction.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestPredicateSiread() {
    TransactionManager manager;
    auto& table_reader = manager.Begin(IsolationLevel::Serializable);
    auto& range_reader = manager.Begin(IsolationLevel::Serializable);
    auto& writer = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterTableRead(table_reader, 7);
    manager.RegisterIndexRead(range_reader, 7, 4, IndexKey(10), true,
                              IndexKey(20), false);
    manager.RegisterTableWrite(writer, 7);
    manager.RegisterIndexWrite(writer, 7, 4, IndexKey(15));
    Check(table_reader.GetOutgoingRwDependencies().count(writer.GetId()) == 1 &&
              range_reader.GetOutgoingRwDependencies().count(writer.GetId()) == 1 &&
              writer.GetIncomingRwDependencies().size() == 2,
          "Predicate writes did not create matching dependencies");

    auto& outside_writer = manager.Begin(IsolationLevel::Serializable);
    manager.RegisterIndexWrite(outside_writer, 7, 4, IndexKey(20));
    Check(range_reader.GetOutgoingRwDependencies().count(outside_writer.GetId()) == 0,
          "Exclusive range endpoint matched a phantom write");
    manager.RegisterIndexWrite(outside_writer, 7, 4, IndexKey(10));
    Check(range_reader.GetOutgoingRwDependencies().count(outside_writer.GetId()) == 1,
          "Inclusive range endpoint missed a phantom write");
    Check(manager.GetPredicateSireadCount() == 2,
          "Predicate SIREAD registry has the wrong size");

    manager.Commit(table_reader);
    manager.Commit(range_reader);
    manager.Commit(writer);
    manager.Abort(outside_writer);
}

}  // namespace

int main() {
    try {
        TestPredicateSiread();
        std::cout << "Predicate and range SIREAD tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
