#include "udb/lock_manager.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace udb;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Function>
void Reject(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected error");
}

void TestTableCompatibilityAndUpgrade() {
    TransactionManager transactions;
    LockManager locks;
    auto& first = transactions.Begin();
    auto& second = transactions.Begin();
    auto& writer = transactions.Begin();
    locks.LockTable(first, LockMode::Shared, 1);
    locks.LockTable(second, LockMode::Shared, 1);
    Check(first.GetSharedTableLocks().count(1) == 1 &&
          second.GetSharedTableLocks().count(1) == 1,
          "Compatible shared table locks were not recorded");

    std::atomic<bool> acquired = false;
    std::atomic<bool> failed = false;
    std::thread blocked([&] {
        try {
            locks.LockTable(writer, LockMode::Exclusive, 1);
            acquired = true;
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(30ms);
    Check(!acquired, "Exclusive table lock bypassed shared holders");
    locks.UnlockTable(first, 1);
    std::this_thread::sleep_for(10ms);
    Check(!acquired, "Exclusive table lock ignored a remaining shared holder");
    locks.UnlockTable(second, 1);
    blocked.join();
    Check(acquired && !failed && writer.GetExclusiveTableLocks().count(1) == 1,
          "Waiting exclusive table lock was not granted");
    locks.UnlockTable(writer, 1);

    auto& upgrader = transactions.Begin();
    auto& peer = transactions.Begin();
    locks.LockTable(upgrader, LockMode::Shared, 2);
    locks.LockTable(peer, LockMode::Shared, 2);
    acquired = false;
    std::thread upgrade([&] {
        try {
            locks.LockTable(upgrader, LockMode::Exclusive, 2);
            acquired = true;
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(30ms);
    Check(!acquired, "Table lock upgrade ignored a shared peer");
    locks.UnlockTable(peer, 2);
    upgrade.join();
    Check(acquired && upgrader.GetSharedTableLocks().count(2) == 0 &&
          upgrader.GetExclusiveTableLocks().count(2) == 1,
          "Table lock upgrade did not replace the held mode");
    locks.LockTable(upgrader, LockMode::Shared, 2);  // X satisfies S.
    locks.UnlockTable(upgrader, 2);
}

void TestRowLocksAndUnlockAll() {
    TransactionManager transactions;
    LockManager locks;
    auto& owner = transactions.Begin();
    auto& waiter = transactions.Begin();
    const RID row{7, 3};
    locks.LockRow(owner, LockMode::Exclusive, 9, row);
    locks.LockTable(owner, LockMode::Shared, 9);
    Check(owner.GetExclusiveRowLocks().count(RowLockId{9, row}) == 1,
          "Exclusive row lock was not recorded");

    // A different row is an independent resource.
    locks.LockRow(waiter, LockMode::Exclusive, 9, RID{7, 4});
    locks.UnlockRow(waiter, 9, RID{7, 4});

    std::atomic<bool> acquired = false;
    std::atomic<bool> failed = false;
    std::thread blocked([&] {
        try {
            locks.LockRow(waiter, LockMode::Shared, 9, row);
            acquired = true;
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(30ms);
    Check(!acquired, "Shared row lock bypassed an exclusive holder");
    locks.UnlockAll(owner);
    blocked.join();
    Check(acquired && !failed && owner.GetSharedTableLocks().empty() &&
          owner.GetExclusiveRowLocks().empty() &&
          waiter.GetSharedRowLocks().count(RowLockId{9, row}) == 1,
          "UnlockAll did not release resources or wake a waiter");
    locks.UnlockAll(waiter);

    Reject([&] { locks.UnlockRow(owner, 9, row); });
    Reject([&] { locks.LockRow(owner, LockMode::Shared, 9, RID{-1, 0}); });
    transactions.Commit(owner);
    Reject([&] { locks.LockTable(owner, LockMode::Shared, 1); });
}

}  // namespace

int main() {
    try {
        TestTableCompatibilityAndUpgrade();
        TestRowLocksAndUnlockAll();
        std::cout << "Lock manager tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
