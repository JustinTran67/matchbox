#include "generator/market_simulator.hpp"

#include "generator/seed_stream.hpp"

namespace matchbox {

MarketSimulator::MarketSimulator(const MarketSimulatorConfig& config, std::uint64_t seed)
    : noise_(config.noise, nth_stream(seed, 0), ids_),
      informed_(config.informed, nth_stream(seed, 1)),
      rng_(nth_stream(seed, 2)),
      informed_probability_(config.informed_probability) {}

OrderGenerator::Action MarketSimulator::next(const OrderBook& book) {
  if (std::bernoulli_distribution(informed_probability_)(rng_)) {
    OrderGenerator::Action action;
    action.kind = OrderGenerator::Action::Kind::Submit;
    action.order = informed_.next_order(book, ids_.take());
    return action;
  }

  // The fundamental is a property of the world, not of this trader's activity, so it moves
  // on every step. Without this, a run with a lower informed ratio would also get a slower
  // fundamental, and price-tracking comparisons across ratios would be meaningless.
  informed_.advance();
  return noise_.next(book);
}

}  // namespace matchbox
