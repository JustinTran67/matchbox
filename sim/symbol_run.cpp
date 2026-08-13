#include "symbol_run.hpp"

#include <cstdlib>
#include <memory>

#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"

namespace matchbox::sim {

SymbolStats run_symbol(const SymbolRunConfig& config) {
  Engine engine(std::make_unique<FifoMatcher>());
  MarketSimulator market(config.market, config.seed);

  SymbolStats stats;
  stats.symbol = config.symbol;
  stats.informed_probability = config.market.informed_probability;

  double spread_total = 0.0;
  double resting_total = 0.0;
  std::size_t resting_samples = 0;
  double deviation_total = 0.0;

  for (std::size_t step = 0; step < config.steps; ++step) {
    const OrderGenerator::Action action = market.next(engine.book());

    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      if (engine.cancel(action.cancel_id)) {
        ++stats.cancels;
      }
    } else {
      const ExecutionReport report = engine.submit(action.order);
      ++stats.submissions;
      stats.trades += report.trades.size();
      if (report.resting_quantity > 0) {
        ++stats.passive_posts;
      }
      if (report.filled_quantity > 0) {
        ++stats.crossing_submissions;
      }

      // The fundamental has already advanced for this step, so this is the mispricing the
      // trade actually executed at.
      const Price truth = market.true_price();
      for (const Trade& trade : report.trades) {
        deviation_total += static_cast<double>(std::llabs(trade.price - truth));
      }
    }

    if (step % config.sample_interval == 0) {
      const std::optional<Price> bid = engine.book().best_price(Side::Buy);
      const std::optional<Price> ask = engine.book().best_price(Side::Sell);
      if (bid.has_value() && ask.has_value()) {
        spread_total += static_cast<double>(*ask - *bid);
        ++stats.spread_samples;
      }
      resting_total += static_cast<double>(engine.book().resting_order_count());
      ++resting_samples;
    }
  }

  stats.final_resting = engine.book().resting_order_count();
  stats.mean_spread =
      stats.spread_samples == 0 ? 0.0 : spread_total / static_cast<double>(stats.spread_samples);
  stats.mean_resting_orders =
      resting_samples == 0 ? 0.0 : resting_total / static_cast<double>(resting_samples);
  stats.mean_abs_price_deviation =
      stats.trades == 0 ? 0.0 : deviation_total / static_cast<double>(stats.trades);
  return stats;
}

}  // namespace matchbox::sim
