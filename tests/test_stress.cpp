#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>

#include "generator/order_generator.hpp"
#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"

using namespace matchbox;

namespace {

// An independent model of what the book should contain, built only from execution
// reports. It deliberately shares no code with OrderBook: checking the engine against its
// own front() would let a bug in the book's queue ordering satisfy the matcher that reads
// it. Cancelled and filled orders are pruned lazily when a queue front is inspected.
class BookModel {
 public:
  void rest(OrderId id, Side side, Price price, Quantity quantity) {
    live_.emplace(id, Entry{quantity, price, side});
    (side == Side::Buy ? bids_[price] : asks_[price]).push_back(id);
  }

  bool live(OrderId id) const { return live_.count(id) != 0; }

  void drop(OrderId id) { live_.erase(id); }

  Quantity quantity_of(OrderId id) const {
    const auto entry = live_.find(id);
    return entry == live_.end() ? 0 : entry->second.quantity;
  }

  void reduce(OrderId id, Quantity quantity) {
    const auto entry = live_.find(id);
    entry->second.quantity -= quantity;
    if (entry->second.quantity == 0) {
      live_.erase(entry);
    }
  }

  // The order that price-time priority requires the next trade on `side` to hit.
  std::optional<std::pair<Price, OrderId>> best_resting(Side side) {
    return side == Side::Buy ? best_of(bids_) : best_of(asks_);
  }

  std::size_t size() const { return live_.size(); }

  bool agrees_with(const OrderBook& book) const {
    if (book.resting_order_count() != live_.size()) {
      return false;
    }
    for (const auto& [id, entry] : live_) {
      const Order* order = book.find(id);
      if (order == nullptr || order->quantity != entry.quantity || order->price != entry.price ||
          order->side != entry.side) {
        return false;
      }
    }
    return true;
  }

 private:
  struct Entry {
    Quantity quantity;
    Price price;
    Side side;
  };

  template <typename Levels>
  std::optional<std::pair<Price, OrderId>> best_of(Levels& levels) {
    while (!levels.empty()) {
      auto level = levels.begin();
      std::deque<OrderId>& queue = level->second;
      while (!queue.empty() && live_.count(queue.front()) == 0) {
        queue.pop_front();
      }
      if (queue.empty()) {
        levels.erase(level);
        continue;
      }
      return std::make_pair(level->first, queue.front());
    }
    return std::nullopt;
  }

  std::unordered_map<OrderId, Entry> live_;
  std::map<Price, std::deque<OrderId>, std::greater<Price>> bids_;
  std::map<Price, std::deque<OrderId>, std::less<Price>> asks_;
};

struct StressStats {
  std::size_t trades{0};
  std::size_t successful_cancels{0};
  std::size_t rejected_cancels{0};
  std::size_t submissions{0};
  std::size_t max_trades_per_order{0};
  std::size_t final_resting_orders{0};
  std::size_t peak_resting_orders{0};
  Quantity traded_quantity{0};
};

void run_stress(const GeneratorConfig& config, std::uint64_t seed, std::size_t steps,
                StressStats& stats) {
  Engine engine(std::make_unique<FifoMatcher>());
  OrderGenerator generator(config, seed);
  BookModel model;

  constexpr std::size_t kReconcileInterval = 10'000;

  for (std::size_t step = 0; step < steps; ++step) {
    const OrderGenerator::Action action = generator.next(engine.book());

    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      const bool cancelled = engine.cancel(action.cancel_id);
      if (cancelled != model.live(action.cancel_id)) {
        FAIL("cancel outcome disagrees with the model at step " << step << " for order "
                                                                << action.cancel_id);
      }
      if (cancelled) {
        model.drop(action.cancel_id);
        ++stats.successful_cancels;
      } else {
        ++stats.rejected_cancels;
      }
    } else {
      const Order submitted = action.order;
      const Side resting_side = opposite(submitted.side);
      const ExecutionReport report = engine.submit(submitted);
      ++stats.submissions;

      if (report.filled_quantity + report.resting_quantity + report.cancelled_quantity !=
          submitted.quantity) {
        FAIL("quantity not conserved for order " << submitted.id << " at step " << step);
      }
      if (submitted.type == OrderType::Market && report.resting_quantity != 0) {
        FAIL("market order " << submitted.id << " rested at step " << step);
      }

      Quantity filled = 0;
      for (std::size_t index = 0; index < report.trades.size(); ++index) {
        const Trade& trade = report.trades[index];

        if (trade.quantity == 0 || trade.taker_id != submitted.id ||
            trade.maker_id == submitted.id) {
          FAIL("malformed trade for order " << submitted.id << " at step " << step);
        }
        if (submitted.type == OrderType::Limit) {
          const bool within_limit = submitted.side == Side::Buy ? trade.price <= submitted.price
                                                                : trade.price >= submitted.price;
          if (!within_limit) {
            FAIL("trade at " << trade.price << " violates limit " << submitted.price << " at step "
                             << step);
          }
        }
        if (index > 0) {
          // A sweep consumes the far side best-first, so prices can only get worse for
          // the taker as it walks deeper into the book.
          const Price previous = report.trades[index - 1].price;
          const bool ordered =
              submitted.side == Side::Buy ? trade.price >= previous : trade.price <= previous;
          if (!ordered) {
            FAIL("sweep prices out of order at step " << step);
          }
        }

        // Price-time priority, checked against the model rather than the engine: the
        // counterparty must be the oldest order resting at the best available price.
        const std::optional<std::pair<Price, OrderId>> expected = model.best_resting(resting_side);
        if (!expected.has_value()) {
          FAIL("trade against an empty side at step " << step);
        }
        if (expected->first != trade.price) {
          FAIL("trade at " << trade.price << " but best resting price is " << expected->first
                           << " at step " << step);
        }
        if (expected->second != trade.maker_id) {
          FAIL("trade hit order " << trade.maker_id << " ahead of older order " << expected->second
                                  << " at step " << step);
        }
        if (model.quantity_of(trade.maker_id) < trade.quantity) {
          FAIL("trade consumed more than order " << trade.maker_id << " holds at step " << step);
        }
        model.reduce(trade.maker_id, trade.quantity);

        filled += trade.quantity;
        stats.traded_quantity += trade.quantity;
        ++stats.trades;
      }

      if (filled != report.filled_quantity) {
        FAIL("trade quantities disagree with the report at step " << step);
      }
      stats.max_trades_per_order = std::max(stats.max_trades_per_order, report.trades.size());

      if (report.resting_quantity > 0) {
        if (model.live(submitted.id)) {
          FAIL("duplicate order id " << submitted.id << " rested at step " << step);
        }
        model.rest(submitted.id, submitted.side, submitted.price, report.resting_quantity);
      }
    }

    if (engine.book().crossed()) {
      FAIL("book crossed at step " << step << ": bid " << *engine.book().best_price(Side::Buy)
                                   << " >= ask " << *engine.book().best_price(Side::Sell));
    }

    if (step % kReconcileInterval == 0 && !model.agrees_with(engine.book())) {
      FAIL("model diverged from the book at step " << step);
    }

    stats.peak_resting_orders =
        std::max(stats.peak_resting_orders, engine.book().resting_order_count());
  }

  stats.final_resting_orders = engine.book().resting_order_count();

  REQUIRE(model.agrees_with(engine.book()));
  REQUIRE_FALSE(engine.book().crossed());
}

}  // namespace

TEST_CASE("invariants hold across a long randomised order stream") {
  const std::uint64_t seed = GENERATE(1u, 7u, 42u, 2024u);

  GeneratorConfig config;
  StressStats stats;
  run_stress(config, seed, 100'000, stats);

  REQUIRE(stats.submissions > 0);
  REQUIRE(stats.trades > 0);
  REQUIRE(stats.successful_cancels > 0);
  // Guards against the stress run degenerating into a near-empty book, which would leave
  // the deep-book and multi-level sweep paths of the matcher effectively untested.
  REQUIRE(stats.peak_resting_orders > 1'000);
  REQUIRE(stats.max_trades_per_order > 1);
}

TEST_CASE("invariants hold when order flow is aggressive and trade-heavy") {
  GeneratorConfig config;
  config.price_stddev_ticks = 3.0;
  config.aggressive_probability = 0.45;
  config.market_order_probability = 0.25;
  config.max_aggression_ticks = 8;
  config.cancel_probability = 0.05;

  StressStats stats;
  run_stress(config, 99, 100'000, stats);

  REQUIRE(stats.trades > stats.submissions / 4);
}

TEST_CASE("invariants hold when order flow is passive and the book grows deep") {
  GeneratorConfig config;
  config.price_stddev_ticks = 60.0;
  config.aggressive_probability = 0.02;
  config.market_order_probability = 0.01;
  config.cancel_probability = 0.10;

  StressStats stats;
  run_stress(config, 31'337, 100'000, stats);

  REQUIRE(stats.successful_cancels > 0);
  REQUIRE(stats.final_resting_orders > 10'000);
}
