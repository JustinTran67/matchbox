#include <catch2/catch_test_macros.hpp>

#include "orderbook/order_book.hpp"

using namespace matchbox;

namespace {

Order limit(OrderId id, Side side, Price price, Quantity quantity) {
  return Order{id, side, OrderType::Limit, price, quantity, id};
}

}  // namespace

TEST_CASE("best price is the highest bid and the lowest ask") {
  OrderBook book;
  book.insert(limit(1, Side::Buy, 100, 10));
  book.insert(limit(2, Side::Buy, 102, 10));
  book.insert(limit(3, Side::Buy, 101, 10));
  book.insert(limit(4, Side::Sell, 108, 10));
  book.insert(limit(5, Side::Sell, 105, 10));
  book.insert(limit(6, Side::Sell, 107, 10));

  REQUIRE(book.best_price(Side::Buy) == 102);
  REQUIRE(book.best_price(Side::Sell) == 105);
  REQUIRE(book.level_count(Side::Buy) == 3);
  REQUIRE(book.level_count(Side::Sell) == 3);
  REQUIRE_FALSE(book.crossed());
}

TEST_CASE("an empty side reports no best price") {
  OrderBook book;
  REQUIRE_FALSE(book.best_price(Side::Buy).has_value());
  REQUIRE(book.best_level(Side::Sell) == nullptr);
  REQUIRE_FALSE(book.crossed());
}

TEST_CASE("orders at one price are queued in arrival order") {
  OrderBook book;
  book.insert(limit(1, Side::Buy, 100, 10));
  book.insert(limit(2, Side::Buy, 100, 20));
  book.insert(limit(3, Side::Buy, 100, 30));

  const PriceLevel* level = book.best_level(Side::Buy);
  REQUIRE(level != nullptr);
  REQUIRE(level->order_count() == 3);
  REQUIRE(level->total_quantity() == 60);
  REQUIRE(level->front().id == 1);

  std::vector<OrderId> queued;
  for (const Order& order : *level) {
    queued.push_back(order.id);
  }
  REQUIRE(queued == std::vector<OrderId>{1, 2, 3});
}

TEST_CASE("cancel removes only the targeted order") {
  OrderBook book;
  book.insert(limit(1, Side::Buy, 100, 10));
  book.insert(limit(2, Side::Buy, 100, 20));
  book.insert(limit(3, Side::Buy, 100, 30));

  REQUIRE(book.cancel(2));

  REQUIRE(book.resting_order_count() == 2);
  REQUIRE(book.contains(1));
  REQUIRE_FALSE(book.contains(2));
  REQUIRE(book.contains(3));
  REQUIRE(book.depth_at(Side::Buy, 100) == 40);
  REQUIRE(book.best_level(Side::Buy)->front().id == 1);
}

TEST_CASE("cancelling an unknown or already-cancelled order fails") {
  OrderBook book;
  book.insert(limit(1, Side::Buy, 100, 10));

  REQUIRE_FALSE(book.cancel(99));
  REQUIRE(book.cancel(1));
  REQUIRE_FALSE(book.cancel(1));
  REQUIRE(book.resting_order_count() == 0);
}

TEST_CASE("a price level disappears once its last order leaves") {
  OrderBook book;
  book.insert(limit(1, Side::Buy, 100, 10));
  book.insert(limit(2, Side::Buy, 99, 10));
  REQUIRE(book.best_price(Side::Buy) == 100);

  book.cancel(1);

  REQUIRE(book.level_count(Side::Buy) == 1);
  REQUIRE(book.best_price(Side::Buy) == 99);
  REQUIRE(book.depth_at(Side::Buy, 100) == 0);
}

TEST_CASE("a partial fill reduces depth and keeps the order at the front of the queue") {
  OrderBook book;
  book.insert(limit(1, Side::Sell, 105, 50));
  book.insert(limit(2, Side::Sell, 105, 40));

  book.fill(1, 30);

  REQUIRE(book.depth_at(Side::Sell, 105) == 60);
  REQUIRE(book.find(1) != nullptr);
  REQUIRE(book.find(1)->quantity == 20);
  REQUIRE(book.best_level(Side::Sell)->front().id == 1);
}

TEST_CASE("a complete fill removes the order and its empty level") {
  OrderBook book;
  book.insert(limit(1, Side::Sell, 105, 50));

  book.fill(1, 50);

  REQUIRE(book.resting_order_count() == 0);
  REQUIRE(book.level_count(Side::Sell) == 0);
  REQUIRE(book.find(1) == nullptr);
}

TEST_CASE("book state is preserved across interleaved inserts and cancels") {
  OrderBook book;
  for (OrderId id = 1; id <= 10; ++id) {
    book.insert(limit(id, id % 2 == 0 ? Side::Buy : Side::Sell,
                      id % 2 == 0 ? 100 - static_cast<Price>(id) : 110 + static_cast<Price>(id),
                      id * 10));
  }
  REQUIRE(book.resting_order_count() == 10);

  for (OrderId id = 1; id <= 10; id += 2) {
    REQUIRE(book.cancel(id));
  }

  REQUIRE(book.resting_order_count() == 5);
  REQUIRE(book.level_count(Side::Sell) == 0);
  REQUIRE(book.level_count(Side::Buy) == 5);
  REQUIRE(book.best_price(Side::Buy) == 98);
}
