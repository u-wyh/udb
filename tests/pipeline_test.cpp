#include "udb/sql/operator.h"

#include <iostream>

namespace {
using namespace udb;
using namespace udb::sql;
void Check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}

class CountedRows final : public ExecutionOperator {
public:
    CountedRows(std::vector<Tuple> rows, std::size_t& calls) : rows_(std::move(rows)), calls_(calls) {}
    void Init() override { position_ = 0; calls_ = 0; initialized_ = true; }
    std::optional<Tuple> Next() override {
        RequireInitialized();
        ++calls_;
        if (position_ == rows_.size()) { return std::nullopt; }
        return rows_[position_++];
    }
private:
    std::vector<Tuple> rows_;
    std::size_t& calls_;
    std::size_t position_ = 0;
};

void Run(JoinAlgorithm algorithm) {
    const Schema single({Column("k", TypeId::INTEGER)});
    const Schema joined({Column("l.k", TypeId::INTEGER), Column("r.k", TypeId::INTEGER)});
    const std::vector<Tuple> left = {Tuple(single, {Value::Integer(1)}), Tuple(single, {Value::Integer(2)}),
        Tuple(single, {Value::Integer(2)}), Tuple(single, {Value::Null(TypeId::INTEGER)})};
    const std::vector<Tuple> right = {Tuple(single, {Value::Integer(2)}), Tuple(single, {Value::Integer(1)}),
        Tuple(single, {Value::Integer(2)}), Tuple(single, {Value::Null(TypeId::INTEGER)})};
    auto a = std::make_shared<BoundExpression>(TypeId::INTEGER, BoundColumnExpression{0});
    auto b = std::make_shared<BoundExpression>(TypeId::INTEGER, BoundColumnExpression{1});
    auto equality = std::make_shared<BoundExpression>(TypeId::BOOLEAN,
        BoundComparisonExpression{ComparisonOperator::Equal, a, b});
    std::size_t left_calls = 0;
    std::size_t right_calls = 0;
    auto make_join = [&] {
        return std::make_unique<JoinOperator>(std::make_unique<CountedRows>(left, left_calls),
            std::make_unique<CountedRows>(right, right_calls), joined, 1, equality, algorithm);
    };
    LimitOperator first(make_join(), 1, 0);
    const auto one = first.Execute();
    Check(one.size() == 1 && left_calls == 1, "Join eagerly consumed left input");
    Check(right_calls == (algorithm == JoinAlgorithm::Hash ? 5U : 2U), "Unexpected right reads");
    auto join = make_join();
    Check(join->Execute().size() == 5 && join->Execute().size() == 5, "Join reset or multiplicity is wrong");
    const Schema grouped({Column("k", TypeId::INTEGER), Column("count", TypeId::BIGINT)});
    auto aggregate = std::make_unique<AggregateOperator>(make_join(), joined, grouped,
        std::vector<PlanAggregate>{{AggregateType::Count, std::nullopt, TypeId::BOOLEAN}}, 0, true);
    auto sorted = std::make_unique<SortOperator>(std::move(aggregate), std::vector<PlanOrderBy>{{1, false}});
    auto limited = std::make_unique<LimitOperator>(std::move(sorted), 1, 0);
    ProjectionOperator result(std::move(limited), grouped, {0, 1}, {});
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto rows = result.Execute();
        Check(rows.size() == 1 && rows[0].GetValue(0) == Value::Integer(2) &&
              rows[0].GetValue(1) == Value::BigInt(4), "Join/aggregate/sort pipeline changed result");
    }
    AggregateOperator empty(std::make_unique<MaterializedOperator>(std::vector<Tuple>{}), single,
        Schema({Column("count", TypeId::BIGINT)}),
        {{AggregateType::Count, std::nullopt, TypeId::BOOLEAN}}, std::nullopt, false);
    Check(empty.Execute()[0].GetValue(0) == Value::BigInt(0), "Empty global aggregate is wrong");
    SortOperator nulls(std::make_unique<MaterializedOperator>(left), {{0, false}});
    const auto rows = nulls.Execute();
    Check(rows.size() == 4 && rows[0].GetValue(0) == Value::Integer(2) && rows[3].GetValue(0).IsNull(),
          "Sort NULL handling is wrong");
}
}  // namespace

int main() {
    try {
        Run(JoinAlgorithm::NestedLoop);
        Run(JoinAlgorithm::Hash);
        std::cout << "Unified pipeline tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
