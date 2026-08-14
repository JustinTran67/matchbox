#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "matching/matching_strategy.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

// The disposition of one submitted order. filled + resting + cancelled always equals the
// submitted quantity, which is what makes order conservation checkable from outside.
struct ExecutionReport {
  OrderId order_id{0};
  std::vector<Trade> trades;
  Quantity filled_quantity{0};
  Quantity resting_quantity{0};
  Quantity cancelled_quantity{0};
  // Set when the order was refused outright and never reached a book. The order id is the
  // book's identity for an order, so a duplicate cannot be admitted: the id index would
  // keep only one of them while both sat in the price level, leaving liquidity that can
  // still trade but can never be cancelled.
  bool rejected{false};
};

class Engine {
 public:
  explicit Engine(std::unique_ptr<MatchingStrategy> strategy);

  ExecutionReport submit(Order order);
  bool cancel(OrderId id);

  const OrderBook& book() const { return book_; }
  std::string_view strategy_name() const { return strategy_->name(); }

 private:
  std::unique_ptr<MatchingStrategy> strategy_;
  OrderBook book_;
  Sequence next_sequence_{1};
};

}  // namespace matchbox
