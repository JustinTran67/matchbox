#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "generator/informed_trader.hpp"
#include "generator/market_simulator.hpp"
#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"
#include "symbol_run.hpp"

using namespace matchbox;
using matchbox::sim::SymbolRunConfig;
using matchbox::sim::SymbolStats;

namespace {

// Everything a run produced: the exact trade sequence and the exact book left behind.
// Two runs of one symbol agree only if no hidden state leaked in from elsewhere.
struct SymbolTrace {
  std::vector<Trade> trades;
  std::map<OrderId, std::pair<Price, Quantity>> resting;
  std::size_t resting_count{0};
  Price final_true_price{0};
  // Recorded rather than asserted, because trace_symbol runs on worker threads and Catch2's
  // assertion machinery is not thread-safe - checking it there is itself a data race.
  bool self_consistent{true};
};

bool same_trade(const Trade& a, const Trade& b) {
  return a.taker_id == b.taker_id && a.maker_id == b.maker_id && a.price == b.price &&
         a.quantity == b.quantity;
}

bool operator==(const SymbolTrace& a, const SymbolTrace& b) {
  return a.resting_count == b.resting_count && a.final_true_price == b.final_true_price &&
         a.resting == b.resting && a.trades.size() == b.trades.size() &&
         std::equal(a.trades.begin(), a.trades.end(), b.trades.begin(), same_trade);
}

SymbolTrace trace_symbol(const MarketSimulatorConfig& config, std::uint64_t seed,
                         std::size_t steps) {
  Engine engine(std::make_unique<FifoMatcher>());
  MarketSimulator market(config, seed);
  SymbolTrace trace;

  for (std::size_t step = 0; step < steps; ++step) {
    const OrderGenerator::Action action = market.next(engine.book());

    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      if (engine.cancel(action.cancel_id)) {
        trace.resting.erase(action.cancel_id);
      }
      continue;
    }

    const Order submitted = action.order;
    const ExecutionReport report = engine.submit(submitted);
    for (const Trade& trade : report.trades) {
      trace.trades.push_back(trade);
      const auto maker = trace.resting.find(trade.maker_id);
      if (maker == trace.resting.end() || maker->second.second < trade.quantity) {
        trace.self_consistent = false;
        continue;
      }
      maker->second.second -= trade.quantity;
      if (maker->second.second == 0) {
        trace.resting.erase(maker);
      }
    }
    if (report.resting_quantity > 0) {
      trace.resting.emplace(submitted.id,
                            std::make_pair(submitted.price, report.resting_quantity));
    }
  }

  trace.resting_count = engine.book().resting_order_count();
  trace.final_true_price = market.true_price();
  if (trace.resting_count != trace.resting.size()) {
    trace.self_consistent = false;
  }
  return trace;
}

MarketSimulatorConfig config_with(double informed_probability) {
  MarketSimulatorConfig config;
  config.informed_probability = informed_probability;
  return config;
}

}  // namespace

TEST_CASE("a symbol's outcome does not depend on what runs alongside it", "[concurrency]") {
  constexpr std::size_t kSteps = 20'000;
  constexpr std::uint64_t kTargetSeed = 4242;
  const MarketSimulatorConfig target = config_with(0.25);

  const SymbolTrace baseline = trace_symbol(target, kTargetSeed, kSteps);
  REQUIRE(baseline.self_consistent);
  REQUIRE(baseline.trades.size() > 0);
  REQUIRE(baseline.resting_count > 0);

  // Four threads replay the target symbol while four others run unrelated symbols, so any
  // shared counter, shared RNG, or other hidden global would perturb the target's result.
  constexpr std::size_t kTargetThreads = 4;
  constexpr std::size_t kNoiseThreads = 4;
  std::vector<SymbolTrace> traces(kTargetThreads + kNoiseThreads);
  std::vector<std::thread> threads;

  for (std::size_t i = 0; i < kTargetThreads; ++i) {
    threads.emplace_back([i, &traces, &target] {
      traces[i] = trace_symbol(target, kTargetSeed, kSteps);
    });
  }
  for (std::size_t i = 0; i < kNoiseThreads; ++i) {
    threads.emplace_back([i, &traces] {
      const MarketSimulatorConfig other = config_with(0.1 * static_cast<double>(i + 1));
      traces[kTargetThreads + i] = trace_symbol(other, 900 + i, kSteps);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (std::size_t i = 0; i < traces.size(); ++i) {
    INFO("replica " << i);
    REQUIRE(traces[i].self_consistent);
  }
  for (std::size_t i = 0; i < kTargetThreads; ++i) {
    INFO("target replica " << i);
    REQUIRE(traces[i] == baseline);
  }
}

TEST_CASE("informed order flow pulls traded prices toward the fundamental", "[concurrency]") {
  constexpr std::size_t kSteps = 200'000;

  SymbolRunConfig noise_only;
  noise_only.symbol = "NOISE";
  noise_only.seed = 12'345;
  noise_only.steps = kSteps;
  noise_only.market.informed_probability = 0.0;

  SymbolRunConfig informed = noise_only;
  informed.symbol = "INFORMED";
  informed.market.informed_probability = 0.6;

  const SymbolStats without = matchbox::sim::run_symbol(noise_only);
  const SymbolStats with = matchbox::sim::run_symbol(informed);

  REQUIRE(without.trades > 1'000);
  REQUIRE(with.trades > 1'000);

  INFO("noise-only |px-true| = " << without.mean_abs_price_deviation);
  INFO("informed  |px-true| = " << with.mean_abs_price_deviation);

  // Threshold deliberately loose: the effect measured is roughly an order of magnitude,
  // so half is comfortably clear of run-to-run noise without being a hair's-width call.
  REQUIRE(with.mean_abs_price_deviation < without.mean_abs_price_deviation * 0.5);
}

TEST_CASE("an informed trader buys when the fundamental is above the mid and sells below", "[concurrency]") {
  InformedTraderConfig config;
  config.true_price = 10'000;
  config.true_price_drift_ticks = 0.0;

  Engine engine(std::make_unique<FifoMatcher>());
  engine.submit(Order{1, Side::Buy, OrderType::Limit, 9'000, 100, 0});
  engine.submit(Order{2, Side::Sell, OrderType::Limit, 9'010, 100, 0});

  InformedTrader trader(config, 7);
  REQUIRE(trader.observed_mid(engine.book()) == 9'005);

  const Order order = trader.next_order(engine.book(), 99);
  REQUIRE(order.side == Side::Buy);
  REQUIRE(order.type == OrderType::Limit);
  REQUIRE(order.id == 99);

  InformedTraderConfig below = config;
  below.true_price = 8'000;
  InformedTrader bearish(below, 7);
  REQUIRE(bearish.next_order(engine.book(), 100).side == Side::Sell);
}

TEST_CASE("an informed trader reaches only part of the way to the fundamental", "[concurrency]") {
  InformedTraderConfig config;
  config.true_price = 10'000;
  config.true_price_drift_ticks = 0.0;
  config.correction_aggression = 0.5;

  Engine engine(std::make_unique<FifoMatcher>());
  engine.submit(Order{1, Side::Buy, OrderType::Limit, 9'000, 100, 0});
  engine.submit(Order{2, Side::Sell, OrderType::Limit, 9'010, 100, 0});

  InformedTrader trader(config, 7);
  const Order order = trader.next_order(engine.book(), 99);

  // Anchored on the ask it would cross (9010), reaching half of the way to 10000.
  REQUIRE(order.price == 9'505);
  REQUIRE(order.price < config.true_price);
}

TEST_CASE("an informed trader quotes at fair value into an empty book", "[concurrency]") {
  InformedTraderConfig config;
  config.true_price = 10'000;
  config.true_price_drift_ticks = 0.0;

  OrderBook empty;
  InformedTrader trader(config, 7);
  REQUIRE(trader.observed_mid(empty) == 10'000);

  const Order order = trader.next_order(empty, 1);
  REQUIRE(order.price == 10'000);
  REQUIRE(order.quantity > 0);
}

TEST_CASE("the fundamental advances once per step whichever population acts", "[concurrency]") {
  MarketSimulatorConfig never = config_with(0.0);
  MarketSimulatorConfig always = config_with(1.0);

  Engine engine(std::make_unique<FifoMatcher>());
  MarketSimulator quiet(never, 5150);
  MarketSimulator busy(always, 5150);

  constexpr std::size_t kSteps = 500;
  for (std::size_t i = 0; i < kSteps; ++i) {
    quiet.next(engine.book());
  }
  Engine other(std::make_unique<FifoMatcher>());
  for (std::size_t i = 0; i < kSteps; ++i) {
    busy.next(other.book());
  }

  // Same seed, same number of steps: the fundamental is driven by the world, not by how
  // often the informed trader happens to be chosen.
  REQUIRE(quiet.true_price() == busy.true_price());
  REQUIRE(quiet.true_price() != never.informed.true_price);
}

TEST_CASE("order ids stay unique and increasing across both trader populations", "[concurrency]") {
  MarketSimulatorConfig config = config_with(0.5);
  Engine engine(std::make_unique<FifoMatcher>());
  MarketSimulator market(config, 31);

  OrderId previous = 0;
  std::size_t submissions = 0;
  for (std::size_t step = 0; step < 20'000; ++step) {
    const OrderGenerator::Action action = market.next(engine.book());
    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      engine.cancel(action.cancel_id);
      continue;
    }
    REQUIRE(action.order.id > previous);
    previous = action.order.id;
    ++submissions;
    engine.submit(action.order);
  }
  REQUIRE(submissions > 1'000);
  REQUIRE(previous == submissions);
}
