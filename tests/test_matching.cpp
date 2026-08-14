#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"

using namespace matchbox;

namespace {

Engine make_engine() { return Engine(std::make_unique<FifoMatcher>()); }

Order limit(OrderId id, Side side, Price price, Quantity quantity) {
  return Order{id, side, OrderType::Limit, price, quantity, 0};
}

Order market(OrderId id, Side side, Quantity quantity) {
  return Order{id, side, OrderType::Market, 0, quantity, 0};
}

}  // namespace

TEST_CASE("a limit order that does not cross rests in full") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 50));

  const ExecutionReport report = engine.submit(limit(2, Side::Buy, 104, 30));

  REQUIRE(report.trades.empty());
  REQUIRE(report.filled_quantity == 0);
  REQUIRE(report.resting_quantity == 30);
  REQUIRE(engine.book().best_price(Side::Buy) == 104);
  REQUIRE_FALSE(engine.book().crossed());
}

TEST_CASE("an aggressive order trades at the resting order's price") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 50));

  const ExecutionReport report = engine.submit(limit(2, Side::Buy, 110, 50));

  REQUIRE(report.trades.size() == 1);
  REQUIRE(report.trades[0].price == 105);
  REQUIRE(report.trades[0].quantity == 50);
  REQUIRE(report.trades[0].maker_id == 1);
  REQUIRE(report.trades[0].taker_id == 2);
  REQUIRE(report.filled_quantity == 50);
  REQUIRE(report.resting_quantity == 0);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("orders at the same price fill in arrival order") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));
  engine.submit(limit(3, Side::Sell, 105, 10));

  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 105, 25));

  REQUIRE(report.trades.size() == 3);
  REQUIRE(report.trades[0].maker_id == 1);
  REQUIRE(report.trades[1].maker_id == 2);
  REQUIRE(report.trades[2].maker_id == 3);
  REQUIRE(report.trades[2].quantity == 5);
  REQUIRE(engine.book().find(3)->quantity == 5);
}

TEST_CASE("a taker sweeps price levels best-first") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 107, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));
  engine.submit(limit(3, Side::Sell, 106, 10));

  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 107, 30));

  REQUIRE(report.trades.size() == 3);
  REQUIRE(report.trades[0].price == 105);
  REQUIRE(report.trades[1].price == 106);
  REQUIRE(report.trades[2].price == 107);
  REQUIRE(report.filled_quantity == 30);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("a taker stops at its limit price and rests the remainder") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 108, 10));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 106, 25));

  REQUIRE(report.trades.size() == 1);
  REQUIRE(report.trades[0].price == 105);
  REQUIRE(report.filled_quantity == 10);
  REQUIRE(report.resting_quantity == 15);
  REQUIRE(engine.book().best_price(Side::Buy) == 106);
  REQUIRE(engine.book().best_price(Side::Sell) == 108);
  REQUIRE_FALSE(engine.book().crossed());
}

TEST_CASE("a market order sweeps available liquidity and never rests") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 200, 10));

  const ExecutionReport report = engine.submit(market(3, Side::Buy, 15));

  REQUIRE(report.trades.size() == 2);
  REQUIRE(report.trades[0].price == 105);
  REQUIRE(report.trades[1].price == 200);
  REQUIRE(report.filled_quantity == 15);
  REQUIRE(report.resting_quantity == 0);
  REQUIRE_FALSE(engine.book().contains(3));
}

TEST_CASE("a market order with insufficient liquidity cancels its remainder") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));

  const ExecutionReport report = engine.submit(market(2, Side::Buy, 40));

  REQUIRE(report.filled_quantity == 10);
  REQUIRE(report.cancelled_quantity == 30);
  REQUIRE(report.resting_quantity == 0);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("a market order against an empty book fills nothing") {
  Engine engine = make_engine();

  const ExecutionReport report = engine.submit(market(1, Side::Sell, 25));

  REQUIRE(report.trades.empty());
  REQUIRE(report.cancelled_quantity == 25);
  REQUIRE(engine.book().resting_order_count() == 0);
}

TEST_CASE("a partially filled resting order keeps its queue position") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 100));
  engine.submit(limit(2, Side::Sell, 105, 100));

  engine.submit(limit(3, Side::Buy, 105, 40));
  const ExecutionReport report = engine.submit(limit(4, Side::Buy, 105, 40));

  REQUIRE(report.trades.size() == 1);
  REQUIRE(report.trades[0].maker_id == 1);
  REQUIRE(engine.book().find(1)->quantity == 20);
  REQUIRE(engine.book().find(2)->quantity == 100);
}

TEST_CASE("a sell taker walks down the bids and trades at each bid price") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Buy, 100, 10));
  engine.submit(limit(2, Side::Buy, 102, 10));
  engine.submit(limit(3, Side::Buy, 101, 10));

  const ExecutionReport report = engine.submit(limit(4, Side::Sell, 100, 25));

  REQUIRE(report.trades.size() == 3);
  REQUIRE(report.trades[0].price == 102);
  REQUIRE(report.trades[1].price == 101);
  REQUIRE(report.trades[2].price == 100);
  REQUIRE(report.filled_quantity == 25);
  REQUIRE(engine.book().depth_at(Side::Buy, 100) == 5);
}

TEST_CASE("a cancelled order no longer provides liquidity") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));

  REQUIRE(engine.cancel(1));

  const ExecutionReport report = engine.submit(limit(3, Side::Buy, 105, 10));

  REQUIRE(report.trades.size() == 1);
  REQUIRE(report.trades[0].maker_id == 2);
}

TEST_CASE("cancelling a fully filled order fails") {
  Engine engine = make_engine();
  engine.submit(limit(1, Side::Sell, 105, 10));
  engine.submit(limit(2, Side::Buy, 105, 10));

  REQUIRE_FALSE(engine.cancel(1));
  REQUIRE_FALSE(engine.cancel(2));
}

TEST_CASE("the strategy interface reports the active algorithm") {
  const Engine engine = make_engine();
  REQUIRE(engine.strategy_name() == "price-time");
}

TEST_CASE("a duplicate order id is rejected rather than corrupting the book") {
  Engine engine = make_engine();
  const ExecutionReport first = engine.submit(limit(7, Side::Buy, 100, 50));
  REQUIRE_FALSE(first.rejected);
  REQUIRE(first.resting_quantity == 50);

  // Ids come from outside the process, so this has to hold in optimised builds too, where
  // NDEBUG strips asserts. Admitting the duplicate would leave the id index tracking one
  // order while the price level held two - liquidity that trades but cannot be cancelled.
  const ExecutionReport duplicate = engine.submit(limit(7, Side::Buy, 100, 50));
  REQUIRE(duplicate.rejected);
  REQUIRE(duplicate.trades.empty());
  REQUIRE(duplicate.resting_quantity == 0);
  REQUIRE(duplicate.cancelled_quantity == 50);

  REQUIRE(engine.book().resting_order_count() == 1);
  REQUIRE(engine.book().depth_at(Side::Buy, 100) == 50);

  REQUIRE(engine.cancel(7));
  REQUIRE(engine.book().resting_order_count() == 0);
  REQUIRE(engine.book().depth_at(Side::Buy, 100) == 0);
}

TEST_CASE("a zero-quantity order is rejected") {
  Engine engine = make_engine();
  const ExecutionReport report = engine.submit(limit(1, Side::Buy, 100, 0));
  REQUIRE(report.rejected);
  REQUIRE(engine.book().resting_order_count() == 0);
}
