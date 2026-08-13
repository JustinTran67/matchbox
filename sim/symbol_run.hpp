#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "generator/market_simulator.hpp"
#include "orderbook/types.hpp"

namespace matchbox::sim {

struct SymbolStats {
  std::string symbol;
  double informed_probability{0.0};
  double mean_spread{0.0};
  double mean_resting_orders{0.0};
  double mean_abs_price_deviation{0.0};
  std::size_t trades{0};
  std::size_t submissions{0};
  std::size_t cancels{0};
  std::size_t final_resting{0};
  std::size_t spread_samples{0};
  // Separates the two ways a submission can end, which is what distinguishes adverse
  // selection from simply posting less passive liquidity in the first place.
  std::size_t passive_posts{0};
  std::size_t crossing_submissions{0};
};

struct SymbolRunConfig {
  std::string symbol;
  MarketSimulatorConfig market;
  std::uint64_t seed{1};
  std::size_t steps{200'000};
  std::size_t sample_interval{100};
};

// Drives one symbol end to end. Touches nothing outside its own arguments, which is what
// makes it safe to call from many threads at once.
SymbolStats run_symbol(const SymbolRunConfig& config);

}  // namespace matchbox::sim
