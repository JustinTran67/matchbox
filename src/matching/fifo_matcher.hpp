#pragma once

#include <string_view>
#include <vector>

#include "matching/matching_strategy.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

// Price-time priority: best price first, and within a price the earliest arrival first.
class FifoMatcher final : public MatchingStrategy {
 public:
  std::string_view name() const override { return "price-time"; }

  void match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) override;
};

}  // namespace matchbox
