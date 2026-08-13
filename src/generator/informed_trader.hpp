#pragma once

#include <cstdint>
#include <random>

#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

struct InformedTraderConfig {
  Price true_price{10'000};
  double true_price_drift_ticks{0.3};
  double correction_aggression{0.6};
  Quantity min_quantity{1};
  Quantity max_quantity{300};
  Price min_price{1};
};

// A trader that can see a fundamental the market cannot. The fundamental is a random walk
// advanced once per simulation step; when the book's mid drifts away from it, this trader
// posts on the side that pulls the mid back.
//
// Pricing anchors on the touch the order would have to cross - the ask for a buy, the bid
// for a sell - and reaches `correction_aggression` of the way from there to the true
// price. So an order crosses only when the fundamental is already through that touch, and
// even then it stops short of the fundamental rather than sweeping the whole book.
//
// Fallbacks: with one side empty the mid is that side's best price; with the book empty
// the mid is the true price itself, so the trader sees no mispricing and posts passively
// at fair value, seeding an empty book rather than doing nothing.
class InformedTrader {
 public:
  InformedTrader(const InformedTraderConfig& config, std::uint64_t seed);

  // Steps the fundamental. Callers that do not ask for an order on a given step must still
  // call this, so the fundamental evolves at a rate independent of how often this trader
  // acts - otherwise runs at different informed ratios would not be comparable.
  void advance();

  // Advances the fundamental, then returns this trader's order for the current book.
  Order next_order(const OrderBook& book, OrderId id);

  Price true_price() const { return true_price_; }
  Price observed_mid(const OrderBook& book) const;

 private:
  Price anchor_for(const OrderBook& book, Side side, Price mid) const;

  InformedTraderConfig config_;
  // Two streams, deliberately. The fundamental must advance at exactly one draw per step
  // whatever else this trader does; sharing one generator with the order attributes would
  // make the fundamental's path depend on how often the trader is asked to quote, which
  // would make runs at different informed ratios incomparable.
  std::mt19937_64 price_rng_;
  std::mt19937_64 order_rng_;
  Price true_price_;
};

}  // namespace matchbox
