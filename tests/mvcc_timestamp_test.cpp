#include "udb/execution_context.h"
#include "udb/transaction.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void TestSnapshotsAndWatermark() {
    const auto initial = TransactionManager::GetLastCommitTimestamp();
    Check(TransactionManager::GetWatermark() == initial,
          "Empty active set watermark is not the latest commit");

    TransactionManager first;
    TransactionManager second;
    auto& long_reader = first.Begin();
    auto& fast = second.Begin();
    Check(long_reader.GetReadTimestamp() == initial &&
          fast.GetReadTimestamp() == initial &&
          !long_reader.GetCommitTimestamp() &&
          TransactionManager::GetWatermark() == initial,
          "BEGIN did not capture the current commit timestamp");

    second.Commit(fast);
    Check(fast.GetCommitTimestamp() == initial + 1 &&
          TransactionManager::GetLastCommitTimestamp() == initial + 1 &&
          TransactionManager::GetWatermark() == initial,
          "Commit timestamp or old snapshot watermark is wrong");

    auto& newer = second.Begin();
    Check(newer.GetReadTimestamp() == initial + 1,
          "New transaction did not see the published commit timestamp");
    second.Abort(newer);
    Check(!newer.GetCommitTimestamp() && TransactionManager::GetWatermark() == initial,
          "Aborted transaction received a commit timestamp or changed old watermark");

    first.Commit(long_reader);
    Check(long_reader.GetCommitTimestamp() == initial + 2 &&
          TransactionManager::GetWatermark() == initial + 2,
          "Watermark did not advance after the oldest snapshot ended");
}

void TestConcurrentCommits() {
    constexpr std::size_t kTransactions = 12;
    const auto base = TransactionManager::GetLastCommitTimestamp();
    TransactionManager watermark_manager;
    auto& old_snapshot = watermark_manager.Begin();
    std::vector<std::unique_ptr<TransactionManager>> managers;
    std::vector<Transaction*> transactions;
    for (std::size_t i = 0; i < kTransactions; ++i) {
        managers.push_back(std::make_unique<TransactionManager>());
        transactions.push_back(&managers.back()->Begin());
        Check(transactions.back()->GetReadTimestamp() == base,
              "Concurrent transaction read timestamp changed before commits");
    }

    std::atomic<bool> start = false;
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < kTransactions; ++i) {
        workers.emplace_back([&, i] {
            while (!start.load()) { std::this_thread::yield(); }
            managers[i]->Commit(*transactions[i]);
        });
    }
    start = true;
    for (auto& worker : workers) { worker.join(); }

    std::vector<timestamp_t> commits;
    for (const auto* transaction : transactions) {
        Check(transaction->GetCommitTimestamp().has_value(),
              "Concurrent commit did not assign a timestamp");
        commits.push_back(*transaction->GetCommitTimestamp());
    }
    std::sort(commits.begin(), commits.end());
    for (std::size_t i = 0; i < commits.size(); ++i) {
        Check(commits[i] == base + i + 1,
              "Concurrent commit timestamps are not unique and contiguous");
    }
    Check(TransactionManager::GetWatermark() == base,
          "Concurrent commits advanced past an active old snapshot");
    watermark_manager.Commit(old_snapshot);
    Check(old_snapshot.GetCommitTimestamp() == base + kTransactions + 1 &&
          TransactionManager::GetWatermark() == base + kTransactions + 1,
          "Watermark did not catch up after concurrent commits");
}

void TestManagerDestructionAndContext() {
    const auto base = TransactionManager::GetLastCommitTimestamp();
    {
        TransactionManager abandoned;
        auto& transaction = abandoned.Begin(IsolationLevel::ReadCommitted);
        ExecutionContext context(transaction);
        Check(context.GetTransaction().GetReadTimestamp() == base &&
              TransactionManager::GetWatermark() == base,
              "Execution context lost its transaction snapshot");
    }
    Check(TransactionManager::GetWatermark() == base,
          "Destroyed manager left a stale active snapshot in the watermark");
}

}  // namespace

int main() {
    try {
        TestSnapshotsAndWatermark();
        TestConcurrentCommits();
        TestManagerDestructionAndContext();
        std::cout << "MVCC timestamp core tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
