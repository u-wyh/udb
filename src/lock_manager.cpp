#include "udb/lock_manager.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace udb {

bool LockManager::Compatible(LockMode left, LockMode right) {
    return left == LockMode::Shared && right == LockMode::Shared;
}

LockManager::RequestIterator LockManager::Find(Queue& queue, Transaction& transaction) {
    return std::find_if(queue.requests.begin(), queue.requests.end(),
                        [&](const Request& request) { return request.transaction == &transaction; });
}

bool LockManager::CanGrant(const Queue& queue,
                           std::list<Request>::const_iterator request) {
    if (queue.upgrader != nullptr && queue.upgrader != request->transaction) { return false; }
    for (auto current = queue.requests.begin(); current != request; ++current) {
        if (!current->granted) { return false; }
    }
    for (const auto& current : queue.requests) {
        if (&current != &*request && current.granted &&
            !Compatible(current.mode, request->mode)) {
            return false;
        }
    }
    return true;
}

bool LockManager::CanUpgrade(const Queue& queue, const Transaction& transaction) {
    return std::none_of(queue.requests.begin(), queue.requests.end(),
                        [&](const Request& request) {
                            return request.transaction != &transaction && request.granted;
                        });
}

void LockManager::LockTable(Transaction& transaction, LockMode mode, table_id_t table_id) {
    if (!transaction.IsActive()) { throw std::logic_error("Cannot lock for an inactive transaction"); }
    std::unique_lock<std::mutex> lock(mutex_);
    auto& queue = table_queues_[table_id];
    if (!queue) { queue = std::make_shared<Queue>(); }
    LockTableLocked(lock, transaction, mode, table_id, queue);
}

void LockManager::LockTableLocked(std::unique_lock<std::mutex>& lock, Transaction& transaction,
                                  LockMode mode, table_id_t table_id,
                                  const std::shared_ptr<Queue>& queue) {
    auto request = Find(*queue, transaction);
    if (request != queue->requests.end()) {
        if (!request->granted) { throw std::logic_error("Transaction already has a waiting table lock"); }
        if (request->mode == LockMode::Exclusive || mode == LockMode::Shared) { return; }
        if (queue->upgrader != nullptr && queue->upgrader != &transaction) {
            throw std::logic_error("Another transaction is already upgrading this table lock");
        }
        queue->upgrader = &transaction;
        request->upgrading = true;
        queue->condition.wait(lock, [&] { return CanUpgrade(*queue, transaction); });
        request->mode = LockMode::Exclusive;
        request->upgrading = false;
        queue->upgrader = nullptr;
        transaction.shared_table_locks_.erase(table_id);
        transaction.exclusive_table_locks_.insert(table_id);
        queue->condition.notify_all();
        return;
    }
    queue->requests.push_back(Request{&transaction, mode});
    request = std::prev(queue->requests.end());
    queue->condition.wait(lock, [&] { return CanGrant(*queue, request); });
    request->granted = true;
    if (mode == LockMode::Shared) { transaction.shared_table_locks_.insert(table_id); }
    else { transaction.exclusive_table_locks_.insert(table_id); }
    queue->condition.notify_all();
}

void LockManager::UnlockTable(Transaction& transaction, table_id_t table_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = table_queues_.find(table_id);
    if (found == table_queues_.end()) { throw std::logic_error("Transaction does not hold table lock"); }
    auto request = Find(*found->second, transaction);
    if (request == found->second->requests.end() || !request->granted || request->upgrading) {
        throw std::logic_error("Transaction does not hold releasable table lock");
    }
    found->second->requests.erase(request);
    transaction.shared_table_locks_.erase(table_id);
    transaction.exclusive_table_locks_.erase(table_id);
    found->second->condition.notify_all();
    if (found->second->requests.empty()) { table_queues_.erase(found); }
}

void LockManager::LockRow(Transaction& transaction, LockMode mode,
                          table_id_t table_id, RID rid) {
    if (!transaction.IsActive()) { throw std::logic_error("Cannot lock for an inactive transaction"); }
    if (rid.page_id < 0) { throw std::invalid_argument("Row lock RID is invalid"); }
    std::unique_lock<std::mutex> lock(mutex_);
    const RowLockId resource{table_id, rid};
    auto& queue = row_queues_[resource];
    if (!queue) { queue = std::make_shared<Queue>(); }
    LockRowLocked(lock, transaction, mode, resource, queue);
}

void LockManager::LockRowLocked(std::unique_lock<std::mutex>& lock, Transaction& transaction,
                                LockMode mode, RowLockId resource,
                                const std::shared_ptr<Queue>& queue) {
    auto request = Find(*queue, transaction);
    if (request != queue->requests.end()) {
        if (!request->granted) { throw std::logic_error("Transaction already has a waiting row lock"); }
        if (request->mode == LockMode::Exclusive || mode == LockMode::Shared) { return; }
        if (queue->upgrader != nullptr && queue->upgrader != &transaction) {
            throw std::logic_error("Another transaction is already upgrading this row lock");
        }
        queue->upgrader = &transaction;
        request->upgrading = true;
        queue->condition.wait(lock, [&] { return CanUpgrade(*queue, transaction); });
        request->mode = LockMode::Exclusive;
        request->upgrading = false;
        queue->upgrader = nullptr;
        transaction.shared_row_locks_.erase(resource);
        transaction.exclusive_row_locks_.insert(resource);
        queue->condition.notify_all();
        return;
    }
    queue->requests.push_back(Request{&transaction, mode});
    request = std::prev(queue->requests.end());
    queue->condition.wait(lock, [&] { return CanGrant(*queue, request); });
    request->granted = true;
    if (mode == LockMode::Shared) { transaction.shared_row_locks_.insert(resource); }
    else { transaction.exclusive_row_locks_.insert(resource); }
    queue->condition.notify_all();
}

void LockManager::UnlockRow(Transaction& transaction, table_id_t table_id, RID rid) {
    std::lock_guard<std::mutex> lock(mutex_);
    const RowLockId resource{table_id, rid};
    const auto found = row_queues_.find(resource);
    if (found == row_queues_.end()) { throw std::logic_error("Transaction does not hold row lock"); }
    auto request = Find(*found->second, transaction);
    if (request == found->second->requests.end() || !request->granted || request->upgrading) {
        throw std::logic_error("Transaction does not hold releasable row lock");
    }
    found->second->requests.erase(request);
    transaction.shared_row_locks_.erase(resource);
    transaction.exclusive_row_locks_.erase(resource);
    found->second->condition.notify_all();
    if (found->second->requests.empty()) { row_queues_.erase(found); }
}

void LockManager::UnlockAll(Transaction& transaction) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto queue = table_queues_.begin(); queue != table_queues_.end();) {
        if (queue->second->upgrader == &transaction) { queue->second->upgrader = nullptr; }
        auto request = Find(*queue->second, transaction);
        if (request != queue->second->requests.end()) { queue->second->requests.erase(request); }
        queue->second->condition.notify_all();
        if (queue->second->requests.empty()) { queue = table_queues_.erase(queue); }
        else { ++queue; }
    }
    for (auto queue = row_queues_.begin(); queue != row_queues_.end();) {
        if (queue->second->upgrader == &transaction) { queue->second->upgrader = nullptr; }
        auto request = Find(*queue->second, transaction);
        if (request != queue->second->requests.end()) { queue->second->requests.erase(request); }
        queue->second->condition.notify_all();
        if (queue->second->requests.empty()) { queue = row_queues_.erase(queue); }
        else { ++queue; }
    }
    transaction.shared_table_locks_.clear();
    transaction.exclusive_table_locks_.clear();
    transaction.shared_row_locks_.clear();
    transaction.exclusive_row_locks_.clear();
}

}  // namespace udb
