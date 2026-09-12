#pragma once

#include <cstdint>
#include <map>
#include <memory>

namespace udb {

using transaction_id_t = std::uint64_t;

enum class TransactionState { Active, Committed, Aborted };

class Transaction {
public:
    transaction_id_t GetId() const { return id_; }
    TransactionState GetState() const { return state_; }
    bool IsActive() const { return state_ == TransactionState::Active; }

private:
    friend class TransactionManager;
    explicit Transaction(transaction_id_t id) : id_(id) {}
    transaction_id_t id_;
    TransactionState state_ = TransactionState::Active;
};

class TransactionManager {
public:
    Transaction& Begin();
    void Commit(Transaction& transaction);
    void Abort(Transaction& transaction);
    Transaction& GetTransaction(transaction_id_t id);
    const Transaction& GetTransaction(transaction_id_t id) const;
    std::size_t GetActiveCount() const;

private:
    Transaction& RequireManaged(Transaction& transaction);
    transaction_id_t next_id_ = 0;
    std::map<transaction_id_t, std::unique_ptr<Transaction>> transactions_;
};

}  // namespace udb
