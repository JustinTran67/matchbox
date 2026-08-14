#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "matching/engine.hpp"

namespace RdKafka {
class KafkaConsumer;
class Producer;
class Message;
}  // namespace RdKafka

namespace matchbox::service {

struct EngineServiceConfig {
  std::string brokers{"localhost:9092"};
  std::string orders_topic{"matchbox.orders"};
  std::string trades_topic{"matchbox.trades"};
  std::string group_id{"matchbox-engine"};
  std::vector<std::int32_t> partitions;
  int poll_timeout_ms{500};
  // Offsets are committed in batches, but never ahead of an acknowledged produce - see
  // commit_progress().
  std::size_t commit_interval{100};
};

struct EngineServiceStats {
  std::size_t orders_consumed{0};
  std::size_t submits{0};
  std::size_t cancels{0};
  std::size_t trades_published{0};
  std::size_t malformed{0};
  std::size_t symbols{0};
};

// Owns whole partitions of the orders topic and the books for every symbol on them.
//
// Partitions are assigned explicitly rather than subscribed to. A consumer group would
// rebalance, and rebalancing has a window in which ownership of a partition can overlap
// between members. For a stateless consumer that is harmless; here a partition carries
// mutable in-memory books, so an overlap means two processes matching against two
// divergent copies of one symbol's book, and publishing conflicting trades for it.
class EngineService {
 public:
  explicit EngineService(EngineServiceConfig config);
  ~EngineService();

  EngineService(const EngineService&) = delete;
  EngineService& operator=(const EngineService&) = delete;

  bool start(std::string& error);
  void run(const std::atomic<bool>& running);
  void stop();

  bool ready() const { return ready_.load(); }
  EngineServiceStats stats() const { return stats_; }

 private:
  // A book per symbol, not per partition: symbols are mapped onto partitions by hashing
  // the key, so a partition can carry several symbols and must keep their books apart.
  Engine& engine_for(const std::string& symbol);

  void handle(const RdKafka::Message& message);
  bool commit_progress(std::string& error);

  EngineServiceConfig config_;
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
  std::unique_ptr<RdKafka::Producer> producer_;
  std::map<std::string, std::unique_ptr<Engine>> engines_;
  std::atomic<bool> ready_{false};
  EngineServiceStats stats_;
  std::size_t since_commit_{0};
};

}  // namespace matchbox::service
