#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "generator/order_generator.hpp"
#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"
#include "matching/pro_rata_matcher.hpp"

#include "book_model.hpp"

using namespace matchbox;
using matchbox::testing::BookModel;

namespace {

Engine pro_rata_engine() { return Engine(std::make_unique<ProRataMatcher>()); }

Order limit(OrderId id, Side side, Price price, Quantity quantity) {
  return Order{id, side, OrderType::Limit, price, quantity, 0};
}

Order market(OrderId id, Side side, Quantity quantity) {
  return Order{id, side, OrderType::Market, 0, quantity, 0};
}

using Fill = std::pair<OrderId, Quantity>;

std::vector<Fill> fills_of(const ExecutionReport& report) {
  std::vector<Fill> fills;
  fills.reserve(report.trades.size());
  for (const Trade& trade : report.trades) {
    fills.emplace_back(trade.maker_id, trade.quantity);
  }
  return fills;
}

// An independent transcription of the allocation rule, used to check randomised runs. It
// is written from the specification rather than by calling the matcher, so a matcher that
// reads mutated book state, iterates the wrong way, or forgets the per-order cap diverges
// from it. The hand-written cases below are what pin down the rule itself.
std::vector<Fill> expected_allocation(const std::vector<BookModel::RestingOrder>& orders,
                                      Quantity incoming) {
  Quantity level_total = 0;
  for (const BookModel::RestingOrder& order : orders) {
    level_total += order.quantity;
  }

  std::vector<Quantity> allocated(orders.size(), 0);
  if (incoming >= level_total) {
    for (std::size_t i = 0; i < orders.size(); ++i) {
      allocated[i] = orders[i].quantity;
    }
  } else {
    Quantity placed = 0;
    for (std::size_t i = 0; i < orders.size(); ++i) {
      allocated[i] = static_cast<Quantity>(
          (static_cast<unsigned __int128>(incoming) * orders[i].quantity) / level_total);
      placed += allocated[i];
    }
    for (std::size_t i = 0; i < orders.size() && placed < incoming; ++i) {
      if (allocated[i] < orders[i].quantity) {
        ++allocated[i];
        ++placed;
      }
    }
  }

  std::vector<Fill> fills;
  for (std::size_t i = 0; i < orders.size(); ++i) {
    if (allocated[i] > 0) {
      fills.emplace_back(orders[i].id, allocated[i]);
    }
  }
  return fills;
}

}  // namespace

TEST_CASE("pro-rata reports its own algorithm name") {
  const Engine engine = pro_rata_engine();
  REQUIRE(engine.strategy_name() == "pro-rata");
}

TEST_CASE("an incoming order that covers the level fills every resting order in full") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 30));
  engine.submit(limit(2, Side::Sell, 105, 70));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 105, 100));

  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 30}, {2, 70}});
  REQUIRE(report.filled_quantity == 100);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("a partial level is shared in proportion to resting size, not arrival order") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 90));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 105, 50));

  // 50 * 10/100 = 5 and 50 * 90/100 = 45, with nothing left over to round.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 5}, {2, 45}});
  REQUIRE(engine.book().find(1)->quantity == 5);
  REQUIRE(engine.book().find(2)->quantity == 45);
}

TEST_CASE("price-time priority would fill the same level differently") {
  Engine fifo(std::make_unique<FifoMatcher>());
  fifo.submit(limit(1, Side::Sell, 105, 10));
  fifo.submit(limit(2, Side::Sell, 105, 90));

  const ExecutionReport report = fifo.submit(limit(3, Side::Buy, 105, 50));

  // The oldest order is filled to exhaustion first; pro-rata gives it 5 of the same 50.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 10}, {2, 40}});
}

TEST_CASE("rounding leftovers go to the oldest orders first") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));
  engine.submit(limit(3, Side::Sell, 105, 10));

  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 105, 20));

  // Each raw share is 20 * 10/30 = 6.67, so flooring gives 6 apiece and strands 2 units;
  // those go one each to the two oldest orders.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 7}, {2, 7}, {3, 6}});
  REQUIRE(report.filled_quantity == 20);
}

TEST_CASE("uneven sizes and an uneven remainder are allocated exactly") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 100));
  engine.submit(limit(2, Side::Sell, 105, 50));
  engine.submit(limit(3, Side::Sell, 105, 30));

  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 105, 100));

  // Raw shares off a 180 level are 55.6 / 27.8 / 16.7, flooring to 55 / 27 / 16 = 98,
  // and the 2 stranded units go to the two oldest.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 56}, {2, 28}, {3, 16}});
  REQUIRE(report.filled_quantity == 100);
  REQUIRE(engine.book().find(1)->quantity == 44);
  REQUIRE(engine.book().find(2)->quantity == 22);
  REQUIRE(engine.book().find(3)->quantity == 14);
}

TEST_CASE("orders too small to earn a share are left untouched rather than filled for zero") {
  Engine engine = pro_rata_engine();
  for (OrderId id = 1; id <= 5; ++id) {
    engine.submit(limit(id, Side::Sell, 105, 100));
  }

  const ExecutionReport report = engine.submit(limit(6, Side::Buy, 105, 3));

  // Every raw share is 0.6, so the whole incoming quantity is remainder and reaches only
  // the three oldest orders. The other two must not appear as zero-quantity trades.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 1}, {2, 1}, {3, 1}});
  REQUIRE(engine.book().find(4)->quantity == 100);
  REQUIRE(engine.book().find(5)->quantity == 100);
}

TEST_CASE("a remainder unit never pushes an order past what it has resting") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 1));
  engine.submit(limit(2, Side::Sell, 105, 1000));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 105, 500));

  // The tiny order's raw share floors to 0 and it receives the single stranded unit,
  // which is exactly all it has.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 1}, {2, 499}});
  REQUIRE(engine.book().contains(1) == false);
  REQUIRE(engine.book().find(2)->quantity == 501);
}

TEST_CASE("a sweep applies pro-rata at each level in price order") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));
  engine.submit(limit(3, Side::Sell, 106, 100));

  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 106, 60));

  // The 105 level is covered outright, then the remaining 40 is allocated at 106.
  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 10}, {2, 10}, {3, 40}});
  REQUIRE(report.trades[0].price == 105);
  REQUIRE(report.trades[2].price == 106);
  REQUIRE_FALSE(engine.book().crossed());
}

TEST_CASE("a market order is allocated pro-rata and never rests") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 40));
  engine.submit(limit(2, Side::Sell, 105, 60));

  const ExecutionReport report = engine.submit(market(3, Side::Buy, 50));

  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 20}, {2, 30}});
  REQUIRE(report.resting_quantity == 0);
  REQUIRE_FALSE(engine.book().contains(3));
}

TEST_CASE("a market order past the available depth cancels its remainder") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));

  const ExecutionReport report = engine.submit(market(3, Side::Buy, 100));

  REQUIRE(report.filled_quantity == 20);
  REQUIRE(report.cancelled_quantity == 80);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("a taker stops at its limit price and rests the remainder uncrossed") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Sell, 105, 20));
  engine.submit(limit(2, Side::Sell, 108, 20));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 106, 50));

  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 20}});
  REQUIRE(report.resting_quantity == 30);
  REQUIRE(engine.book().best_price(Side::Buy) == 106);
  REQUIRE_FALSE(engine.book().crossed());
}

TEST_CASE("a sell taker is allocated pro-rata across the bids") {
  Engine engine = pro_rata_engine();
  engine.submit(limit(1, Side::Buy, 100, 25));
  engine.submit(limit(2, Side::Buy, 100, 75));

  const ExecutionReport report = engine.submit(limit(3, Side::Sell, 100, 40));

  REQUIRE(fills_of(report) == std::vector<Fill>{{1, 10}, {2, 30}});
  REQUIRE(report.filled_quantity == 40);
}

TEST_CASE("pro-rata holds its invariants across a long randomised order stream") {
  const std::uint64_t seed = GENERATE(3u, 11u, 77u, 4242u);

  GeneratorConfig config;
  Engine engine = pro_rata_engine();
  OrderGenerator generator(config, seed);
  BookModel model;

  constexpr std::size_t kSteps = 100'000;
  constexpr std::size_t kReconcileInterval = 10'000;

  std::size_t trades = 0;
  std::size_t successful_cancels = 0;
  std::size_t multi_maker_fills = 0;
  std::size_t peak_resting = 0;

  for (std::size_t step = 0; step < kSteps; ++step) {
    const OrderGenerator::Action action = generator.next(engine.book());

    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      const bool cancelled = engine.cancel(action.cancel_id);
      if (cancelled != model.live(action.cancel_id)) {
        FAIL("cancel outcome disagrees with the model at step " << step);
      }
      if (cancelled) {
        model.drop(action.cancel_id);
        ++successful_cancels;
      }
    } else {
      const Order submitted = action.order;
      const Side resting_side = opposite(submitted.side);
      const ExecutionReport report = engine.submit(submitted);

      if (report.filled_quantity + report.resting_quantity + report.cancelled_quantity !=
          submitted.quantity) {
        FAIL("quantity not conserved for order " << submitted.id << " at step " << step);
      }
      if (submitted.type == OrderType::Market && report.resting_quantity != 0) {
        FAIL("market order " << submitted.id << " rested at step " << step);
      }

      // Trades arrive grouped by level, so a run of equal prices is exactly one
      // allocation and can be checked against the model as a whole.
      Quantity remaining = submitted.quantity;
      std::size_t index = 0;
      while (index < report.trades.size()) {
        const Price price = report.trades[index].price;
        std::size_t end = index;
        std::vector<Fill> actual;
        while (end < report.trades.size() && report.trades[end].price == price) {
          const Trade& trade = report.trades[end];
          if (trade.quantity == 0 || trade.taker_id != submitted.id ||
              trade.maker_id == submitted.id) {
            FAIL("malformed trade for order " << submitted.id << " at step " << step);
          }
          actual.emplace_back(trade.maker_id, trade.quantity);
          ++end;
        }

        if (submitted.type == OrderType::Limit) {
          const bool within_limit = submitted.side == Side::Buy ? price <= submitted.price
                                                                : price >= submitted.price;
          if (!within_limit) {
            FAIL("trade at " << price << " violates limit " << submitted.price << " at step "
                             << step);
          }
        }
        if (index > 0) {
          const Price previous = report.trades[index - 1].price;
          const bool ordered = submitted.side == Side::Buy ? price >= previous : price <= previous;
          if (!ordered) {
            FAIL("sweep prices out of order at step " << step);
          }
        }

        const std::optional<BookModel::LevelView> level = model.best_level(resting_side);
        if (!level.has_value()) {
          FAIL("trade against an empty side at step " << step);
        }
        if (level->price != price) {
          FAIL("allocated at " << price << " but best resting price is " << level->price
                               << " at step " << step);
        }

        const std::vector<Fill> expected = expected_allocation(level->orders, remaining);
        if (actual != expected) {
          FAIL("pro-rata allocation at price " << price << " step " << step << " gave "
                                               << actual.size() << " fills, model expected "
                                               << expected.size());
        }

        for (const Fill& fill : actual) {
          if (model.quantity_of(fill.first) < fill.second) {
            FAIL("trade consumed more than order " << fill.first << " holds at step " << step);
          }
          model.reduce(fill.first, fill.second);
          remaining -= fill.second;
          ++trades;
        }
        if (actual.size() > 1) {
          ++multi_maker_fills;
        }
        index = end;
      }

      if (submitted.quantity - remaining != report.filled_quantity) {
        FAIL("trade quantities disagree with the report at step " << step);
      }

      if (report.resting_quantity > 0) {
        if (model.live(submitted.id)) {
          FAIL("duplicate order id " << submitted.id << " rested at step " << step);
        }
        model.rest(submitted.id, submitted.side, submitted.price, report.resting_quantity);
      }
    }

    if (engine.book().crossed()) {
      FAIL("book crossed at step " << step);
    }
    if (step % kReconcileInterval == 0 && !model.agrees_with(engine.book())) {
      FAIL("model diverged from the book at step " << step);
    }
    peak_resting = std::max(peak_resting, engine.book().resting_order_count());
  }

  REQUIRE(model.agrees_with(engine.book()));
  REQUIRE_FALSE(engine.book().crossed());
  REQUIRE(trades > 0);
  REQUIRE(successful_cancels > 0);
  REQUIRE(peak_resting > 1'000);
  // Pro-rata's whole point is splitting one taker across several makers; if that never
  // happened the run would not have exercised the allocation path at all.
  REQUIRE(multi_maker_fills > 0);
}
