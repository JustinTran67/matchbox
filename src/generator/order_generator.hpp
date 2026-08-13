#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "generator/order_id_source.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

struct GeneratorConfig {
  Price reference_price{10'000};
  double price_stddev_ticks{12.0};
  double reference_drift_ticks{0.4};
  Price min_half_spread_ticks{1};
  Price max_aggression_ticks{3};
  double aggressive_probability{0.12};
  double market_order_probability{0.06};
  double cancel_probability{0.15};
  Quantity min_quantity{1};
  Quantity max_quantity{500};
  Price min_price{1};
};

// Synthetic order flow. Passive orders are posted away from a slowly drifting reference
// price - bids below it, asks above it - so they rest and build depth, which is how the
// large majority of real order flow behaves. Liquidity is taken by the explicitly
// aggressive and market orders, and by drift carrying the reference through orders that
// are already resting. Phase 3 replaces this with distinct informed and noise traders.
class OrderGenerator {
 public:
  struct Action {
    enum class Kind { Submit, Cancel };

    Kind kind{Kind::Submit};
    Order order{};
    OrderId cancel_id{0};
  };

  OrderGenerator(const GeneratorConfig& config, std::uint64_t seed);
  // Draws ids from a source shared with other trader populations on the same symbol.
  OrderGenerator(const GeneratorConfig& config, std::uint64_t seed, OrderIdSource& ids);

  // ids_ may point at owned_ids_, so copying or moving would leave it dangling.
  OrderGenerator(const OrderGenerator&) = delete;
  OrderGenerator& operator=(const OrderGenerator&) = delete;

  Action next(const OrderBook& book);

  Price reference_price() const { return reference_price_; }

 private:
  Order generate_order(const OrderBook& book);
  std::optional<Price> aggressive_price(const OrderBook& book, Side side);
  Price passive_price(Side side);
  std::optional<OrderId> take_working_order(const OrderBook& book);

  GeneratorConfig config_;
  std::mt19937_64 rng_;
  Price reference_price_;
  OrderIdSource owned_ids_;
  OrderIdSource* ids_;
  std::vector<OrderId> working_orders_;
};

}  // namespace matchbox
