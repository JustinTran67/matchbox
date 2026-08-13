#pragma once

#include <string_view>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

// A limit order accepts any price at least as good as its own; a market order accepts any
// price at all. This is the sole gate on trading, so it is also what keeps the book
// uncrossed: matching stops exactly when the incoming order stops crossing the far touch.
inline bool crosses(const Order& incoming, Price resting_price) {
  if (incoming.type == OrderType::Market) {
    return true;
  }
  return incoming.side == Side::Buy ? incoming.price >= resting_price
                                    : incoming.price <= resting_price;
}

class MatchingStrategy {
 public:
  virtual ~MatchingStrategy() = default;

  virtual std::string_view name() const = 0;

  // Fills `incoming` against resting liquidity, appending each execution to `trades`.
  // On return `incoming.quantity` holds the unfilled remainder, and no resting order
  // crosses it, so the caller may rest that remainder without crossing the book.
  virtual void match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) = 0;
};

}  // namespace matchbox
