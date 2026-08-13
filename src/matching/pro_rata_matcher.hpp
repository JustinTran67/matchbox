#pragma once

#include <string_view>
#include <vector>

#include "matching/matching_strategy.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

// Price priority across levels is identical to price-time matching; the difference is
// confined to how a single level is shared out. An incoming order that cannot absorb a
// level is split across every order resting there in proportion to size, so time priority
// survives only as the tiebreak for the units left over by rounding.
class ProRataMatcher final : public MatchingStrategy {
 public:
  std::string_view name() const override { return "pro-rata"; }

  void match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) override;

 private:
  struct Allocation {
    OrderId id;
    Quantity resting;
    Quantity allocated;
  };

  Quantity allocate(Quantity incoming_quantity, Quantity level_total);

  // Reused across calls so matching does not allocate on the hot path. One buffer per
  // matcher is enough because the engine drives a strategy from a single thread.
  std::vector<Allocation> scratch_;
};

}  // namespace matchbox
