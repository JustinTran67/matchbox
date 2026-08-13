#include "generator/informed_trader.hpp"

#include <algorithm>
#include <cmath>

#include "generator/seed_stream.hpp"

namespace matchbox {
namespace {

Price ticks_from(double value) { return static_cast<Price>(std::llround(value)); }

}  // namespace

InformedTrader::InformedTrader(const InformedTraderConfig& config, std::uint64_t seed)
    : config_(config),
      price_rng_(nth_stream(seed, 0)),
      order_rng_(nth_stream(seed, 1)),
      true_price_(config.true_price) {}

void InformedTrader::advance() {
  std::normal_distribution<double> step(0.0, config_.true_price_drift_ticks);
  true_price_ = std::max(config_.min_price, true_price_ + ticks_from(step(price_rng_)));
}

Price InformedTrader::observed_mid(const OrderBook& book) const {
  const std::optional<Price> bid = book.best_price(Side::Buy);
  const std::optional<Price> ask = book.best_price(Side::Sell);

  if (bid.has_value() && ask.has_value()) {
    return (*bid + *ask) / 2;
  }
  if (bid.has_value()) {
    return *bid;
  }
  if (ask.has_value()) {
    return *ask;
  }
  return true_price_;
}

Price InformedTrader::anchor_for(const OrderBook& book, Side side, Price mid) const {
  const std::optional<Price> touch = book.best_price(opposite(side));
  return touch.value_or(mid);
}

Order InformedTrader::next_order(const OrderBook& book, OrderId id) {
  advance();

  const Price mid = observed_mid(book);

  Order order;
  order.id = id;
  order.type = OrderType::Limit;
  order.quantity =
      std::uniform_int_distribution<Quantity>(config_.min_quantity, config_.max_quantity)(order_rng_);

  if (true_price_ == mid) {
    // No edge to trade on, so this trader simply shows a passive quote at fair value.
    order.side = std::bernoulli_distribution(0.5)(order_rng_) ? Side::Buy : Side::Sell;
    order.price = std::max(config_.min_price, true_price_);
    return order;
  }

  order.side = true_price_ > mid ? Side::Buy : Side::Sell;

  const Price anchor = anchor_for(book, order.side, mid);
  const double reach =
      config_.correction_aggression * static_cast<double>(true_price_ - anchor);
  order.price = std::max(config_.min_price, anchor + ticks_from(reach));
  return order;
}

}  // namespace matchbox
