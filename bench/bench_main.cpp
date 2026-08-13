#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "build_info.hpp"
#include "generator/order_generator.hpp"
#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"
#include "matching/pro_rata_matcher.hpp"

#ifndef MATCHBOX_BENCH_BUILD_TYPE
#define MATCHBOX_BENCH_BUILD_TYPE "unknown"
#endif

using namespace matchbox;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kWarmupActions = 50'000;
constexpr std::size_t kMeasuredActions = 500'000;

enum class Strategy { Fifo, ProRata };

std::unique_ptr<MatchingStrategy> make_strategy(Strategy strategy) {
  if (strategy == Strategy::Fifo) {
    return std::make_unique<FifoMatcher>();
  }
  return std::make_unique<ProRataMatcher>();
}

// The same action stream is replayed against both strategies so the comparison is not
// confounded by different random draws. The stream has to be produced against *some*
// book, because the generator prices aggressive orders off the touch and picks cancel
// targets from resting orders; price-time is used for that, and the resulting difference
// in cancel hit rate is reported per strategy rather than hidden.
std::vector<OrderGenerator::Action> record_stream(const GeneratorConfig& config,
                                                  std::uint64_t seed, std::size_t actions) {
  Engine engine(make_strategy(Strategy::Fifo));
  OrderGenerator generator(config, seed);

  std::vector<OrderGenerator::Action> stream;
  stream.reserve(actions);
  for (std::size_t i = 0; i < actions; ++i) {
    const OrderGenerator::Action action = generator.next(engine.book());
    stream.push_back(action);
    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      engine.cancel(action.cancel_id);
    } else {
      engine.submit(action.order);
    }
  }
  return stream;
}

struct Result {
  double seconds{0.0};
  double throughput{0.0};
  double mean_ns{0.0};
  double trades_per_submission{0.0};
  std::size_t max_makers{0};
  std::uint64_t p50{0};
  std::uint64_t p99{0};
  std::uint64_t p999{0};
  std::size_t trades{0};
  std::size_t submissions{0};
  std::size_t cancels_hit{0};
  std::size_t cancels_missed{0};
  std::size_t final_resting{0};
};

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
  if (sorted.empty()) {
    return 0;
  }
  const std::size_t index =
      std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * sorted.size()));
  return sorted[index];
}

Result run(Strategy strategy, const std::vector<OrderGenerator::Action>& stream,
           std::vector<std::uint64_t>& latencies) {
  Engine warmup(make_strategy(strategy));
  for (std::size_t i = 0; i < std::min(kWarmupActions, stream.size()); ++i) {
    const OrderGenerator::Action& action = stream[i];
    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      warmup.cancel(action.cancel_id);
    } else {
      warmup.submit(action.order);
    }
  }

  Engine engine(make_strategy(strategy));
  Result result;
  latencies.clear();
  latencies.resize(stream.size());

  const Clock::time_point run_start = Clock::now();
  for (std::size_t i = 0; i < stream.size(); ++i) {
    const OrderGenerator::Action& action = stream[i];

    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      const Clock::time_point start = Clock::now();
      const bool cancelled = engine.cancel(action.cancel_id);
      const Clock::time_point end = Clock::now();
      latencies[i] =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         end - start)
                                         .count());
      cancelled ? ++result.cancels_hit : ++result.cancels_missed;
    } else {
      const Clock::time_point start = Clock::now();
      const ExecutionReport report = engine.submit(action.order);
      const Clock::time_point end = Clock::now();
      latencies[i] =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         end - start)
                                         .count());
      result.trades += report.trades.size();
      result.max_makers = std::max(result.max_makers, report.trades.size());
      ++result.submissions;
    }
  }
  const Clock::time_point run_end = Clock::now();

  result.seconds = std::chrono::duration<double>(run_end - run_start).count();
  result.throughput = static_cast<double>(stream.size()) / result.seconds;
  result.mean_ns = result.seconds * 1e9 / static_cast<double>(stream.size());
  result.trades_per_submission =
      result.submissions == 0 ? 0.0
                              : static_cast<double>(result.trades) /
                                    static_cast<double>(result.submissions);
  result.final_resting = engine.book().resting_order_count();

  std::sort(latencies.begin(), latencies.end());
  result.p50 = percentile(latencies, 0.50);
  result.p99 = percentile(latencies, 0.99);
  result.p999 = percentile(latencies, 0.999);
  return result;
}

// Two back-to-back clock reads bracket every measured call. What matters more than their
// cost is the clock's granularity: percentiles below one tick cannot be resolved at all,
// so the reported p50 can be pinned to the floor rather than to real work.
struct ClockProfile {
  std::uint64_t median_overhead_ns;
  std::uint64_t granularity_ns;
};

ClockProfile profile_clock() {
  constexpr std::size_t kSamples = 200'000;
  std::vector<std::uint64_t> samples(kSamples);
  for (std::size_t i = 0; i < kSamples; ++i) {
    const Clock::time_point start = Clock::now();
    const Clock::time_point end = Clock::now();
    samples[i] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  }
  std::sort(samples.begin(), samples.end());

  std::uint64_t granularity = 0;
  for (const std::uint64_t sample : samples) {
    if (sample > 0) {
      granularity = sample;
      break;
    }
  }
  return ClockProfile{samples[samples.size() / 2], granularity};
}

void report(const char* config_name, const char* strategy_name, const Result& result) {
  std::printf("%-11s %-11s %11.0f %8.0f %7llu %7llu %8llu %8.1f %7zu %8zu %8zu\n", config_name,
              strategy_name, result.throughput, result.mean_ns,
              static_cast<unsigned long long>(result.p50),
              static_cast<unsigned long long>(result.p99),
              static_cast<unsigned long long>(result.p999), result.trades_per_submission,
              result.max_makers, result.cancels_hit, result.final_resting);
}

GeneratorConfig aggressive_config() {
  GeneratorConfig config;
  config.price_stddev_ticks = 3.0;
  config.aggressive_probability = 0.45;
  config.market_order_probability = 0.25;
  config.max_aggression_ticks = 8;
  config.cancel_probability = 0.05;
  return config;
}

}  // namespace

int main() {
  std::printf("matchbox benchmark\n");
  std::printf("  build type       : %s\n", MATCHBOX_BENCH_BUILD_TYPE);
  std::printf("  engine assertions: %s\n", assertions_enabled() ? "ON (timings inflated)" : "off");
  std::printf("  actions per run  : %zu measured, %zu warm-up\n", kMeasuredActions,
              kWarmupActions);
  const ClockProfile clock = profile_clock();
  std::printf("  clock overhead   : %llu ns median per measured call\n",
              static_cast<unsigned long long>(clock.median_overhead_ns));
  std::printf("  clock granularity: %llu ns (percentiles below this are not resolvable)\n\n",
              static_cast<unsigned long long>(clock.granularity_ns));

  if (assertions_enabled()) {
    std::printf(
        "  WARNING: configure with -DCMAKE_BUILD_TYPE=Release -DMATCHBOX_ENABLE_ASSERTS=OFF\n"
        "           for numbers worth quoting.\n\n");
  }

  std::printf("%-11s %-11s %11s %8s %7s %7s %8s %8s %7s %8s %8s\n", "config", "strategy",
              "actions/s", "mean ns", "p50 ns", "p99 ns", "p99.9 ns", "fills/or", "maxfill",
              "cancels", "resting");
  std::printf("%s\n", std::string(114, '-').c_str());

  std::vector<std::uint64_t> latencies;
  latencies.reserve(kMeasuredActions);

  const struct {
    const char* name;
    GeneratorConfig config;
    std::uint64_t seed;
  } configs[] = {
      {"default", GeneratorConfig{}, 1},
      {"aggressive", aggressive_config(), 99},
  };

  for (const auto& entry : configs) {
    const std::vector<OrderGenerator::Action> stream =
        record_stream(entry.config, entry.seed, kMeasuredActions);

    report(entry.name, "price-time", run(Strategy::Fifo, stream, latencies));
    report(entry.name, "pro-rata", run(Strategy::ProRata, stream, latencies));
  }

  return 0;
}
