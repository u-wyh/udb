#include "udb/transaction.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try {
        function();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error("Expected error was not thrown");
}

Record Bytes(const std::string& value) {
    return Record(value.data(), value.size());
}

bool Equal(const Record& record, const std::string& value) {
    return record.Size() == value.size() &&
           (value.empty() || std::memcmp(record.Data(), value.data(), value.size()) == 0);
}

void TestVersionChain() {
    TransactionManager manager;
    const RID rid{4, 2};
    auto& first = manager.Begin();
    const auto oldest = manager.AppendUndoRecord(
        first, rid, Bytes(std::string("old\0row", 7)), {3, false});
    const auto newer = manager.AppendUndoRecord(first, rid, Bytes("middle"), {8, true});
    Check(oldest == VersionLink{first.GetId(), 0} &&
              newer == VersionLink{first.GetId(), 1} &&
              first.GetUndoRecordCount() == 2,
          "Undo records are not owned and indexed by their transaction");
    Check(manager.GetVersionLink(rid) == newer, "RID does not point to the newest undo version");

    const auto middle = manager.GetUndoRecord(newer);
    Check(middle.rid == rid && Equal(middle.record, "middle") &&
              middle.meta == TupleMeta{8, true} && middle.previous == oldest,
          "Newest undo record is incomplete");
    const auto old = manager.GetUndoRecord(*middle.previous);
    Check(Equal(old.record, std::string("old\0row", 7)) &&
              old.meta == TupleMeta{3, false} && !old.previous,
          "Oldest undo record did not preserve binary Record and TupleMeta");
    manager.Commit(first);
    Check(manager.GetVersionLink(rid) == newer &&
              Equal(manager.GetUndoRecord(oldest).record, std::string("old\0row", 7)),
          "Commit discarded history needed by older snapshots");

    auto& aborted = manager.Begin();
    const auto aborted_link = manager.AppendUndoRecord(aborted, rid, Bytes("current"), {11, false});
    Check(manager.GetUndoRecord(aborted_link).previous == newer,
          "A later transaction did not extend the existing chain");
    bool visible_during_abort = false;
    manager.Abort(aborted, [&] {
        visible_during_abort = manager.GetVersionLink(rid) == aborted_link &&
                               Equal(manager.GetUndoRecord(aborted_link).record, "current");
    });
    Check(visible_during_abort && manager.GetVersionLink(rid) == newer &&
              aborted.GetUndoRecordCount() == 0,
          "Abort did not restore the prior version-chain head");
    Reject<std::logic_error>([&] {
        manager.AppendUndoRecord(aborted, rid, Bytes("bad"), {});
    });
}

void TestIndependentRidsAndErrors() {
    TransactionManager manager;
    auto& transaction = manager.Begin();
    const RID first{1, 0};
    const RID second{2, 0};
    const auto first_link = manager.AppendUndoRecord(transaction, first, Bytes("a"), {});
    const auto second_link = manager.AppendUndoRecord(transaction, second, Bytes("b"), {5, true});
    Check(manager.GetVersionLink(first) == first_link &&
              manager.GetVersionLink(second) == second_link,
          "Independent RID chains interfered");
    Check(!manager.GetVersionLink({9, 9}), "Missing RID unexpectedly has a version link");
    Reject<std::invalid_argument>([&] {
        manager.AppendUndoRecord(transaction, {-1, 0}, Bytes("bad"), {});
    });
    Reject<std::out_of_range>([&] {
        manager.GetUndoRecord({transaction.GetId(), 99});
    });
    Reject<std::out_of_range>([&] {
        manager.GetUndoRecord({transaction.GetId() + 100, 0});
    });
    manager.Abort(transaction);
    Check(!manager.GetVersionLink(first) && !manager.GetVersionLink(second),
          "Abort left new RID version chains behind");
}

}  // namespace

int main() {
    try {
        TestVersionChain();
        TestIndependentRidsAndErrors();
        std::cout << "Undo version chain tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
