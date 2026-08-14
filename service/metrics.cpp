#include "metrics.hpp"

#include <cstdio>

namespace matchbox::service {
namespace {

void append_counter(std::string& out, const char* name, const char* help,
                    std::size_t value) {
  char scratch[256];
  std::snprintf(scratch, sizeof(scratch), "# HELP %s %s\n# TYPE %s counter\n%s %zu\n", name,
                help, name, name, value);
  out += scratch;
}

void append_gauge(std::string& out, const char* name, const char* help, std::size_t value) {
  char scratch[256];
  std::snprintf(scratch, sizeof(scratch), "# HELP %s %s\n# TYPE %s gauge\n%s %zu\n", name, help,
                name, name, value);
  out += scratch;
}

}  // namespace

void LatencyHistogram::observe(double seconds) {
  for (std::size_t i = 0; i < kLatencyBuckets.size(); ++i) {
    if (seconds <= kLatencyBuckets[i].le) {
      buckets_[i].fetch_add(1, std::memory_order_relaxed);
      break;
    }
    if (i + 1 == kLatencyBuckets.size()) {
      overflow_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const double nanos = seconds * 1e9;
  sum_nanos_.fetch_add(nanos < 0.0 ? 0 : static_cast<std::uint64_t>(nanos),
                       std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
}

LatencySnapshot LatencyHistogram::snapshot() const {
  LatencySnapshot snapshot;
  std::uint64_t running = 0;
  for (std::size_t i = 0; i < kLatencyBuckets.size(); ++i) {
    running += buckets_[i].load(std::memory_order_relaxed);
    snapshot.cumulative[i] = running;
  }
  // Counters are read independently, so a concurrent observe() can land between them.
  // count_ is therefore taken last and clamped up to the cumulative total, keeping
  // _count >= the +Inf bucket rather than letting a scrape expose a decreasing histogram.
  const std::uint64_t overflow = overflow_.load(std::memory_order_relaxed);
  const std::uint64_t observed = count_.load(std::memory_order_relaxed);
  snapshot.count = observed < running + overflow ? running + overflow : observed;
  snapshot.sum_seconds =
      static_cast<double>(sum_nanos_.load(std::memory_order_relaxed)) / 1e9;
  return snapshot;
}

std::string render_metrics(const EngineServiceStats& stats, const LatencySnapshot& latency) {
  std::string out;
  out.reserve(2048);

  append_counter(out, "matchbox_orders_consumed_total",
                 "Order messages consumed from the orders topic.", stats.orders_consumed);
  append_counter(out, "matchbox_submits_total", "Submit actions applied to a book.",
                 stats.submits);
  append_counter(out, "matchbox_cancels_total", "Cancel actions applied to a book.",
                 stats.cancels);
  append_counter(out, "matchbox_trades_published_total",
                 "Trades published to the trades topic.", stats.trades_published);
  append_counter(out, "matchbox_malformed_total",
                 "Messages rejected as malformed before reaching a book.", stats.malformed);
  append_gauge(out, "matchbox_symbols_owned",
               "Distinct symbols with a book on this replica.", stats.symbols);

  out +=
      "# HELP matchbox_submit_latency_seconds Time spent matching one submitted order.\n"
      "# TYPE matchbox_submit_latency_seconds histogram\n";

  char scratch[256];
  for (std::size_t i = 0; i < kLatencyBuckets.size(); ++i) {
    std::snprintf(scratch, sizeof(scratch),
                  "matchbox_submit_latency_seconds_bucket{le=\"%s\"} %llu\n",
                  kLatencyBuckets[i].label,
                  static_cast<unsigned long long>(latency.cumulative[i]));
    out += scratch;
  }
  std::snprintf(scratch, sizeof(scratch),
                "matchbox_submit_latency_seconds_bucket{le=\"+Inf\"} %llu\n"
                "matchbox_submit_latency_seconds_sum %.9f\n"
                "matchbox_submit_latency_seconds_count %llu\n",
                static_cast<unsigned long long>(latency.count), latency.sum_seconds,
                static_cast<unsigned long long>(latency.count));
  out += scratch;

  return out;
}

}  // namespace matchbox::service
