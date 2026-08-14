#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine_service.hpp"
#include "health_server.hpp"
#include "partition_assignment.hpp"

using namespace matchbox::service;

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int) { g_running = false; }

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::string(value);
}

int env_int(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  const std::string pod_name = env_or("POD_NAME", "matchbox-engine-0");
  const int replica_count = env_int("REPLICA_COUNT", 4);
  const int partition_count = env_int("PARTITION_COUNT", 4);

  const std::optional<int> ordinal = ordinal_from_pod_name(pod_name);
  if (!ordinal.has_value()) {
    // Guessing an ordinal would risk two pods claiming one partition, so refuse instead.
    std::fprintf(stderr, "POD_NAME '%s' has no trailing ordinal\n", pod_name.c_str());
    return 1;
  }

  EngineServiceConfig config;
  config.brokers = env_or("KAFKA_BROKERS", "localhost:9092");
  config.orders_topic = env_or("ORDERS_TOPIC", "matchbox.orders");
  config.trades_topic = env_or("TRADES_TOPIC", "matchbox.trades");
  config.group_id = env_or("GROUP_ID", "matchbox-engine");
  config.partitions = partitions_for(*ordinal, replica_count, partition_count);

  if (config.partitions.empty()) {
    std::fprintf(stderr, "ordinal %d owns no partitions of %d across %d replicas\n", *ordinal,
                 partition_count, replica_count);
    return 1;
  }

  std::printf("matchbox-engine pod=%s ordinal=%d brokers=%s partitions=", pod_name.c_str(),
              *ordinal, config.brokers.c_str());
  for (const std::int32_t partition : config.partitions) {
    std::printf("%d ", partition);
  }
  std::printf("\n");
  std::fflush(stdout);

  EngineService service(config);
  HealthServer health(env_int("HEALTH_PORT", 8080), [&service] { return service.ready(); },
                      [&service] { return service.metrics_text(); });
  if (!health.start()) {
    std::fprintf(stderr, "health server failed to bind\n");
    return 1;
  }

  std::string error;
  if (!service.start(error)) {
    std::fprintf(stderr, "service failed to start: %s\n", error.c_str());
    return 1;
  }

  std::printf("assigned and ready\n");
  std::fflush(stdout);

  service.run(g_running);
  service.stop();
  health.stop();

  const EngineServiceStats stats = service.stats();
  std::printf("shutdown: consumed=%zu submits=%zu cancels=%zu trades=%zu malformed=%zu symbols=%zu\n",
              stats.orders_consumed, stats.submits, stats.cancels, stats.trades_published,
              stats.malformed, stats.symbols);
  return 0;
}
