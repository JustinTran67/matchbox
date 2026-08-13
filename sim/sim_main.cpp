#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "symbol_run.hpp"

using namespace matchbox;
using matchbox::sim::SymbolRunConfig;
using matchbox::sim::SymbolStats;

namespace {

constexpr std::size_t kSymbols = 12;
constexpr std::size_t kSteps = 200'000;

std::string symbol_name(std::size_t index) {
  return "SYM" + std::string(index < 10 ? "0" : "") + std::to_string(index);
}

// Every thread owns its whole world - simulator, engine, and one exclusive results slot -
// so nothing is shared and nothing needs a lock until join().
std::vector<SymbolStats> run_batch(double informed_probability, std::uint64_t seed_base) {
  std::vector<SymbolStats> results(kSymbols);
  std::vector<std::thread> threads;
  threads.reserve(kSymbols);

  for (std::size_t i = 0; i < kSymbols; ++i) {
    threads.emplace_back([i, informed_probability, seed_base, &results] {
      SymbolRunConfig config;
      config.symbol = symbol_name(i);
      config.seed = seed_base + i * 7919;
      config.steps = kSteps;
      config.market.informed_probability = informed_probability;
      results[i] = matchbox::sim::run_symbol(config);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return results;
}

SymbolStats mean_of(const std::vector<SymbolStats>& results) {
  SymbolStats mean;
  mean.symbol = "mean";
  if (results.empty()) {
    return mean;
  }
  const double n = static_cast<double>(results.size());
  for (const SymbolStats& stats : results) {
    mean.informed_probability = stats.informed_probability;
    mean.mean_spread += stats.mean_spread / n;
    mean.mean_resting_orders += stats.mean_resting_orders / n;
    mean.mean_abs_price_deviation += stats.mean_abs_price_deviation / n;
    mean.trades += stats.trades;
    mean.submissions += stats.submissions;
    mean.cancels += stats.cancels;
    mean.final_resting += stats.final_resting;
    mean.passive_posts += stats.passive_posts;
    mean.crossing_submissions += stats.crossing_submissions;
  }
  return mean;
}

// Per-symbol spread varies a lot, so a mean across symbols is only worth quoting with the
// dispersion it hides.
template <typename Select>
double sample_sd(const std::vector<SymbolStats>& results, Select select) {
  if (results.size() < 2) {
    return 0.0;
  }
  const double n = static_cast<double>(results.size());
  double mean = 0.0;
  for (const SymbolStats& stats : results) {
    mean += select(stats) / n;
  }
  double sum_squares = 0.0;
  for (const SymbolStats& stats : results) {
    const double delta = select(stats) - mean;
    sum_squares += delta * delta;
  }
  return std::sqrt(sum_squares / (n - 1.0));
}

void print_row(const SymbolStats& stats) {
  std::printf("%-8s %10.2f %12.1f %14.2f %10zu %10zu %10zu\n", stats.symbol.c_str(),
              stats.mean_spread, stats.mean_resting_orders, stats.mean_abs_price_deviation,
              stats.trades, stats.cancels, stats.final_resting);
}

}  // namespace

int main() {
  std::printf("matchbox multi-symbol simulation\n");
  std::printf("  symbols per batch : %zu (one thread each)\n", kSymbols);
  std::printf("  steps per symbol  : %zu\n", kSteps);
  std::printf("  hardware threads  : %u\n\n", std::thread::hardware_concurrency());

  const double ratios[] = {0.0, 0.2, 0.5};

  std::vector<SymbolStats> summary;
  std::vector<double> spread_sd;
  std::vector<double> deviation_sd;
  for (const double ratio : ratios) {
    const std::vector<SymbolStats> results = run_batch(ratio, 1'000);

    std::printf("informed_probability = %.2f\n", ratio);
    std::printf("%-8s %10s %12s %14s %10s %10s %10s\n", "symbol", "spread", "resting",
                "|px-true|", "trades", "cancels", "final");
    std::printf("%s\n", std::string(80, '-').c_str());
    for (const SymbolStats& stats : results) {
      print_row(stats);
    }
    const SymbolStats mean = mean_of(results);
    print_row(mean);
    std::printf("\n");
    summary.push_back(mean);
    spread_sd.push_back(sample_sd(results, [](const SymbolStats& s) { return s.mean_spread; }));
    deviation_sd.push_back(
        sample_sd(results, [](const SymbolStats& s) { return s.mean_abs_price_deviation; }));
  }

  std::printf("summary across informed ratios (sd = spread of per-symbol means)\n");
  std::printf("%-9s %8s %7s %9s %8s %11s %10s %10s\n", "informed", "spread", "sd",
              "resting", "|px-true|", "sd", "passive/k", "cross/k");
  std::printf("%s\n", std::string(80, '-').c_str());
  for (std::size_t i = 0; i < summary.size(); ++i) {
    const SymbolStats& mean = summary[i];
    const double steps = static_cast<double>(kSteps * kSymbols);
    std::printf("%-9.2f %8.2f %7.2f %9.1f %8.2f %11.2f %10.1f %10.1f\n",
                mean.informed_probability, mean.mean_spread, spread_sd[i],
                mean.mean_resting_orders, mean.mean_abs_price_deviation, deviation_sd[i],
                1000.0 * static_cast<double>(mean.passive_posts) / steps,
                1000.0 * static_cast<double>(mean.crossing_submissions) / steps);
  }
  return 0;
}
