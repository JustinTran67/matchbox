#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace matchbox::service {

// Bucket boundaries in seconds, spanning the range Phase 2's benchmark actually measured
// (p50 around 100ns, p99 into the microseconds, pro-rata tails into milliseconds). The
// label is stored alongside the value so the exposition format never depends on how a
// double happens to print.
struct LatencyBucket {
  double le;
  const char* label;
};

inline constexpr std::array<LatencyBucket, 12> kLatencyBuckets{{
    {0.000001, "1e-06"},
    {0.0000025, "2.5e-06"},
    {0.000005, "5e-06"},
    {0.00001, "1e-05"},
    {0.000025, "2.5e-05"},
    {0.00005, "5e-05"},
    {0.0001, "0.0001"},
    {0.00025, "0.00025"},
    {0.0005, "0.0005"},
    {0.001, "0.001"},
    {0.005, "0.005"},
    {0.01, "0.01"},
}};

// Cumulative bucket counts, as Prometheus defines them: cumulative[i] is the number of
// observations less than or equal to kLatencyBuckets[i].le.
struct LatencySnapshot {
  std::array<std::uint64_t, kLatencyBuckets.size()> cumulative{};
  std::uint64_t count{0};
  double sum_seconds{0.0};
};

// Lock-free by construction. The consume loop calls observe() on its hot path while the
// health server thread calls snapshot(); a mutex here would put a lock back into the very
// loop the rest of the design works to keep lock-free.
class LatencyHistogram {
 public:
  void observe(double seconds);
  LatencySnapshot snapshot() const;

 private:
  // Non-cumulative while accumulating - observe() touches exactly one bucket - and summed
  // into cumulative form only when a snapshot is taken.
  std::array<std::atomic<std::uint64_t>, kLatencyBuckets.size()> buckets_{};
  std::atomic<std::uint64_t> overflow_{0};
  std::atomic<std::uint64_t> count_{0};
  // Nanoseconds rather than a floating-point sum, so the accumulation is exact and needs
  // no atomic<double>.
  std::atomic<std::uint64_t> sum_nanos_{0};
};

// A plain snapshot of the service's counters, decoupled from the atomics that produce it
// so it stays copyable and printable.
struct EngineServiceStats {
  std::size_t orders_consumed{0};
  std::size_t submits{0};
  std::size_t cancels{0};
  std::size_t trades_published{0};
  std::size_t malformed{0};
  std::size_t symbols{0};
};

// Pure: takes snapshots, touches no sockets and no Kafka, so it is fully testable without
// a broker or a cluster.
std::string render_metrics(const EngineServiceStats& stats, const LatencySnapshot& latency);

}  // namespace matchbox::service
