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

bool Is(const std::optional<RecordVersion>& version, const std::string& value,
        TupleMeta meta) {
    return version && version->meta == meta && version->record.Size() == value.size() &&
           (value.empty() ||
            std::memcmp(version->record.Data(), value.data(), value.size()) == 0);
}

void TestSnapshotReconstruction() {
    TransactionManager manager;
    const RID rid{10, 3};
    auto& oldest_owner = manager.Begin();
    manager.AppendUndoRecord(oldest_owner, rid, Bytes(std::string("v2\0", 3)), {2, false});
    manager.Commit(oldest_owner);
    auto& newer_owner = manager.Begin();
    manager.AppendUndoRecord(newer_owner, rid, Bytes("v5"), {5, false});
    manager.Commit(newer_owner);

    const auto current = Bytes("v9");
    Check(Is(manager.ReconstructVersion(rid, current, {9, false}, 10), "v9", {9, false}),
          "Snapshot did not use visible current version");
    Check(Is(manager.ReconstructVersion(rid, current, {9, false}, 8), "v5", {5, false}),
          "Snapshot did not reconstruct the nearest visible undo version");
    Check(Is(manager.ReconstructVersion(rid, current, {9, false}, 2),
             std::string("v2\0", 3), {2, false}),
          "Snapshot did not reconstruct the oldest binary version");
    Check(!manager.ReconstructVersion(rid, current, {9, false}, 1),
          "Snapshot before insertion unexpectedly saw a row");
}

void TestTombstonesAndIndependentChains() {
    TransactionManager manager;
    const RID deleted{4, 1};
    auto& owner = manager.Begin();
    manager.AppendUndoRecord(owner, deleted, Bytes("live"), {3, false});
    manager.Commit(owner);
    Check(!manager.ReconstructVersion(deleted, Bytes("dead"), {7, true}, 8),
          "Visible tombstone returned a row");
    Check(Is(manager.ReconstructVersion(deleted, Bytes("dead"), {7, true}, 6),
             "live", {3, false}),
          "Older snapshot could not see the pre-delete version");

    const RID reinserted{5, 2};
    auto& second = manager.Begin();
    manager.AppendUndoRecord(second, reinserted, Bytes("tombstone"), {4, true});
    manager.Commit(second);
    Check(!manager.ReconstructVersion(reinserted, Bytes("new"), {9, false}, 6),
          "Snapshot ignored a visible historical tombstone");
    Check(Is(manager.ReconstructVersion(reinserted, Bytes("new"), {9, false}, 9),
             "new", {9, false}),
          "New snapshot did not see reinserted current row");

    Check(Is(manager.ReconstructVersion({6, 0}, Bytes("legacy"), {}, 0),
             "legacy", {}),
          "Timestamp-zero legacy tuple is not visible");
    Check(!manager.ReconstructVersion({7, 0}, Bytes("future"), {12, false}, 11),
          "Missing undo chain returned a future tuple");
    Reject<std::invalid_argument>([&] {
        manager.ReconstructVersion({-1, 0}, Bytes("bad"), {}, 0);
    });
}

}  // namespace

int main() {
    try {
        TestSnapshotReconstruction();
        TestTombstonesAndIndependentChains();
        std::cout << "Version reconstruction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
