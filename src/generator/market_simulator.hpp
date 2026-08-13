#pragma once

#include <cstdint>
#include <random>

#include "generator/informed_trader.hpp"
#include "generator/order_generator.hpp"
#include "generator/order_id_source.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

struct MarketSimulatorConfig {
  GeneratorConfig noise;
  InformedTraderConfig informed;
  double informed_probability{0.2};
};

// One symbol's whole order flow: uninformed noise traders plus an informed trader that can
// see the fundamental. Everything here is per-instance - the id source, both populations'
// generators, and every RNG - so two simulators never touch shared state and can be driven
// concurrently on separate threads without synchronisation.
class MarketSimulator {
 public:
  MarketSimulator(const MarketSimulatorConfig& config, std::uint64_t seed);

  // Holds a reference into its own members, so neither copying nor moving is safe.
  MarketSimulator(const MarketSimulator&) = delete;
  MarketSimulator& operator=(const MarketSimulator&) = delete;

  OrderGenerator::Action next(const OrderBook& book);

  Price true_price() const { return informed_.true_price(); }
  const InformedTrader& informed() const { return informed_; }

 private:
  // Declared before noise_, which takes a reference to it during construction.
  OrderIdSource ids_;
  OrderGenerator noise_;
  InformedTrader informed_;
  std::mt19937_64 rng_;
  double informed_probability_;
};

}  // namespace matchbox
