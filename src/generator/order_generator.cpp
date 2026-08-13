#include "generator/order_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace matchbox {
namespace {

constexpr int kMaxCancelProbes = 8;

Price ticks_from(double value) { return static_cast<Price>(std::llround(value)); }

}  // namespace

OrderGenerator::OrderGenerator(const GeneratorConfig& config, std::uint64_t seed)
    : config_(config), rng_(seed), reference_price_(config.reference_price), ids_(&owned_ids_) {}

OrderGenerator::OrderGenerator(const GeneratorConfig& config, std::uint64_t seed,
                               OrderIdSource& ids)
    : config_(config), rng_(seed), reference_price_(config.reference_price), ids_(&ids) {}

OrderGenerator::Action OrderGenerator::next(const OrderBook& book) {
  if (std::bernoulli_distribution(config_.cancel_probability)(rng_)) {
    if (const std::optional<OrderId> id = take_working_order(book)) {
      Action action;
      action.kind = Action::Kind::Cancel;
      action.cancel_id = *id;
      return action;
    }
  }

  Action action;
  action.kind = Action::Kind::Submit;
  action.order = generate_order(book);
  if (action.order.type == OrderType::Limit) {
    working_orders_.push_back(action.order.id);
  }
  return action;
}

Order OrderGenerator::generate_order(const OrderBook& book) {
  std::normal_distribution<double> drift(0.0, config_.reference_drift_ticks);
  reference_price_ =
      std::max(config_.min_price, reference_price_ + ticks_from(drift(rng_)));

  Order order;
  order.id = ids_->take();
  order.side = std::bernoulli_distribution(0.5)(rng_) ? Side::Buy : Side::Sell;
  order.quantity =
      std::uniform_int_distribution<Quantity>(config_.min_quantity, config_.max_quantity)(rng_);

  if (std::bernoulli_distribution(config_.market_order_probability)(rng_)) {
    order.type = OrderType::Market;
    return order;
  }

  order.type = OrderType::Limit;
  if (std::bernoulli_distribution(config_.aggressive_probability)(rng_)) {
    if (const std::optional<Price> price = aggressive_price(book, order.side)) {
      order.price = *price;
      return order;
    }
  }
  order.price = passive_price(order.side);
  return order;
}

// Prices through the far touch so the order takes liquidity on arrival, deep enough to
// sweep more than the top level some of the time. Yields nothing when the far side is
// empty, leaving the caller to post passively instead.
std::optional<Price> OrderGenerator::aggressive_price(const OrderBook& book, Side side) {
  const std::optional<Price> touch = book.best_price(opposite(side));
  if (!touch.has_value()) {
    return std::nullopt;
  }

  std::uniform_int_distribution<Price> through(0, config_.max_aggression_ticks);
  const Price offset = through(rng_);
  const Price price = side == Side::Buy ? *touch + offset : *touch - offset;
  return std::max(config_.min_price, price);
}

Price OrderGenerator::passive_price(Side side) {
  std::normal_distribution<double> depth(0.0, config_.price_stddev_ticks);
  const Price offset = config_.min_half_spread_ticks + std::abs(ticks_from(depth(rng_)));
  const Price price = side == Side::Buy ? reference_price_ - offset : reference_price_ + offset;
  return std::max(config_.min_price, price);
}

// Working ids are recorded on submission and are never removed when an order fills, so
// probing doubles as pruning: dead ids are dropped as they are drawn.
std::optional<OrderId> OrderGenerator::take_working_order(const OrderBook& book) {
  for (int probe = 0; probe < kMaxCancelProbes && !working_orders_.empty(); ++probe) {
    std::uniform_int_distribution<std::size_t> pick(0, working_orders_.size() - 1);
    const std::size_t index = pick(rng_);
    const OrderId id = working_orders_[index];

    working_orders_[index] = working_orders_.back();
    working_orders_.pop_back();

    if (book.contains(id)) {
      return id;
    }
  }
  return std::nullopt;
}

}  // namespace matchbox
