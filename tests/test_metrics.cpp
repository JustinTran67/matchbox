#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include "metrics.hpp"

using matchbox::service::EngineServiceStats;
using matchbox::service::kLatencyBuckets;
using matchbox::service::LatencyHistogram;
using matchbox::service::LatencySnapshot;
using matchbox::service::render_metrics;

namespace {

// Prometheus rejects a scrape outright if a metric lacks its declared type, so the
// presence of these lines is part of the contract, not decoration.
bool declares(const std::string& text, const std::string& name, const std::string& type) {
  return text.find("# HELP " + name + " ") != std::string::npos &&
         text.find("# TYPE " + name + " " + type + "\n") != std::string::npos;
}

std::string value_of(const std::string& text, const std::string& line_prefix) {
  const std::size_t start = text.find("\n" + line_prefix + " ");
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t value_start = start + line_prefix.size() + 2;
  const std::size_t end = text.find('\n', value_start);
  return text.substr(value_start, end - value_start);
}

}  // namespace

TEST_CASE("every counter and gauge is declared with a help and type line") {
  const EngineServiceStats stats{12, 9, 3, 40, 1, 2};
  const std::string text = render_metrics(stats, LatencySnapshot{});

  REQUIRE(declares(text, "matchbox_orders_consumed_total", "counter"));
  REQUIRE(declares(text, "matchbox_submits_total", "counter"));
  REQUIRE(declares(text, "matchbox_cancels_total", "counter"));
  REQUIRE(declares(text, "matchbox_trades_published_total", "counter"));
  REQUIRE(declares(text, "matchbox_malformed_total", "counter"));
  REQUIRE(declares(text, "matchbox_symbols_owned", "gauge"));
  REQUIRE(declares(text, "matchbox_submit_latency_seconds", "histogram"));
}

TEST_CASE("counter values are rendered exactly as supplied") {
  const EngineServiceStats stats{12, 9, 3, 40, 1, 2};
  const std::string text = "\n" + render_metrics(stats, LatencySnapshot{});

  REQUIRE(value_of(text, "matchbox_orders_consumed_total") == "12");
  REQUIRE(value_of(text, "matchbox_submits_total") == "9");
  REQUIRE(value_of(text, "matchbox_cancels_total") == "3");
  REQUIRE(value_of(text, "matchbox_trades_published_total") == "40");
  REQUIRE(value_of(text, "matchbox_malformed_total") == "1");
  REQUIRE(value_of(text, "matchbox_symbols_owned") == "2");
}

TEST_CASE("histogram buckets are cumulative and non-decreasing in le") {
  LatencyHistogram histogram;
  histogram.observe(0.0000005);  // below the first boundary
  histogram.observe(0.000002);
  histogram.observe(0.00003);
  histogram.observe(0.0007);
  histogram.observe(0.02);  // beyond the last boundary, so +Inf only

  const LatencySnapshot snapshot = histogram.snapshot();

  for (std::size_t i = 1; i < snapshot.cumulative.size(); ++i) {
    INFO("bucket " << i << " le=" << kLatencyBuckets[i].label);
    REQUIRE(snapshot.cumulative[i] >= snapshot.cumulative[i - 1]);
  }

  REQUIRE(snapshot.cumulative.front() == 1);
  // The 0.02s observation exceeds every boundary, so the last finite bucket holds four.
  REQUIRE(snapshot.cumulative.back() == 4);
  REQUIRE(snapshot.count == 5);
}

TEST_CASE("the +Inf bucket equals the observation count and the sum is consistent") {
  LatencyHistogram histogram;
  histogram.observe(0.000002);
  histogram.observe(0.000004);
  histogram.observe(0.5);

  const LatencySnapshot snapshot = histogram.snapshot();
  const std::string text = "\n" + render_metrics(EngineServiceStats{}, snapshot);

  REQUIRE(value_of(text, "matchbox_submit_latency_seconds_bucket{le=\"+Inf\"}") == "3");
  REQUIRE(value_of(text, "matchbox_submit_latency_seconds_count") == "3");

  // Sum must cover the observation beyond the last bucket too, or rate() over _sum would
  // silently under-report exactly the slow requests that matter most.
  REQUIRE(snapshot.sum_seconds > 0.5);
  REQUIRE(snapshot.sum_seconds < 0.51);
}

TEST_CASE("an observation exactly on a boundary falls into that bucket") {
  LatencyHistogram histogram;
  histogram.observe(kLatencyBuckets[0].le);

  const LatencySnapshot snapshot = histogram.snapshot();
  REQUIRE(snapshot.cumulative[0] == 1);
  REQUIRE(snapshot.count == 1);
}

TEST_CASE("every bucket boundary is emitted, in order, with its own label") {
  const std::string text = render_metrics(EngineServiceStats{}, LatencySnapshot{});

  std::size_t search = 0;
  for (const auto& bucket : kLatencyBuckets) {
    const std::string line =
        "matchbox_submit_latency_seconds_bucket{le=\"" + std::string(bucket.label) + "\"} ";
    const std::size_t found = text.find(line, search);
    INFO("missing or out-of-order boundary " << bucket.label);
    REQUIRE(found != std::string::npos);
    search = found;
  }
  REQUIRE(text.find("le=\"+Inf\"", search) != std::string::npos);
}

TEST_CASE("an untouched histogram renders zeroes rather than nothing") {
  const std::string text = "\n" + render_metrics(EngineServiceStats{}, LatencySnapshot{});

  REQUIRE(value_of(text, "matchbox_submit_latency_seconds_count") == "0");
  REQUIRE(value_of(text, "matchbox_submit_latency_seconds_bucket{le=\"+Inf\"}") == "0");
}
