#include "engine_service.hpp"

#include <librdkafka/rdkafkacpp.h>

#include <chrono>
#include <cstdio>
#include <string_view>
#include <utility>

#include "matching/fifo_matcher.hpp"
#include "wire.hpp"

namespace matchbox::service {
namespace {

// Counts failed deliveries so a produce error cannot be mistaken for an acknowledgement
// and let the offset advance past trades that never reached the topic.
class DeliveryReport : public RdKafka::DeliveryReportCb {
 public:
  void dr_cb(RdKafka::Message& message) override {
    if (message.err() != RdKafka::ERR_NO_ERROR) {
      ++failures_;
      std::fprintf(stderr, "delivery failed: %s\n", message.errstr().c_str());
    }
  }

  std::size_t failures() const { return failures_; }

 private:
  std::size_t failures_{0};
};

DeliveryReport& delivery_report() {
  static DeliveryReport report;
  return report;
}

}  // namespace

EngineService::EngineService(EngineServiceConfig config) : config_(std::move(config)) {}

EngineServiceStats EngineService::stats() const {
  EngineServiceStats snapshot;
  snapshot.orders_consumed = counters_.orders_consumed.load(std::memory_order_relaxed);
  snapshot.submits = counters_.submits.load(std::memory_order_relaxed);
  snapshot.cancels = counters_.cancels.load(std::memory_order_relaxed);
  snapshot.trades_published = counters_.trades_published.load(std::memory_order_relaxed);
  snapshot.malformed = counters_.malformed.load(std::memory_order_relaxed);
  snapshot.symbols = counters_.symbols.load(std::memory_order_relaxed);
  return snapshot;
}

EngineService::~EngineService() { stop(); }

bool EngineService::start(std::string& error) {
  // Separate configs: consumer-only properties on a producer (and vice versa) are silently
  // ignored with a warning, which buries any real misconfiguration in noise.
  std::unique_ptr<RdKafka::Conf> consumer_conf(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  if (consumer_conf->set("bootstrap.servers", config_.brokers, error) != RdKafka::Conf::CONF_OK ||
      consumer_conf->set("group.id", config_.group_id, error) != RdKafka::Conf::CONF_OK ||
      consumer_conf->set("enable.auto.commit", "false", error) != RdKafka::Conf::CONF_OK ||
      consumer_conf->set("auto.offset.reset", "earliest", error) != RdKafka::Conf::CONF_OK) {
    return false;
  }

  std::unique_ptr<RdKafka::Conf> producer_conf(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  if (producer_conf->set("bootstrap.servers", config_.brokers, error) != RdKafka::Conf::CONF_OK ||
      producer_conf->set("dr_cb", &delivery_report(), error) != RdKafka::Conf::CONF_OK) {
    return false;
  }

  consumer_.reset(RdKafka::KafkaConsumer::create(consumer_conf.get(), error));
  if (consumer_ == nullptr) {
    return false;
  }
  producer_.reset(RdKafka::Producer::create(producer_conf.get(), error));
  if (producer_ == nullptr) {
    return false;
  }

  // assign(), not subscribe(): this is the static-ownership decision, and it is why no
  // rebalance callback exists anywhere in this file.
  std::vector<RdKafka::TopicPartition*> assignment;
  assignment.reserve(config_.partitions.size());
  for (const std::int32_t partition : config_.partitions) {
    assignment.push_back(RdKafka::TopicPartition::create(config_.orders_topic, partition,
                                                         RdKafka::Topic::OFFSET_STORED));
  }

  const RdKafka::ErrorCode assigned = consumer_->assign(assignment);
  for (RdKafka::TopicPartition* partition : assignment) {
    delete partition;
  }
  if (assigned != RdKafka::ERR_NO_ERROR) {
    error = RdKafka::err2str(assigned);
    return false;
  }

  ready_ = true;
  return true;
}

Engine& EngineService::engine_for(const std::string& symbol) {
  const auto existing = engines_.find(symbol);
  if (existing != engines_.end()) {
    return *existing->second;
  }
  auto engine = std::make_unique<Engine>(std::make_unique<FifoMatcher>());
  Engine& reference = *engine;
  engines_.emplace(symbol, std::move(engine));
  counters_.symbols.store(engines_.size(), std::memory_order_relaxed);
  return reference;
}

void EngineService::handle(const RdKafka::Message& message) {
  const std::string_view payload(static_cast<const char*>(message.payload()), message.len());
  const std::optional<OrderMessage> order = decode_order(payload);
  if (!order.has_value()) {
    counters_.malformed.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  counters_.orders_consumed.fetch_add(1, std::memory_order_relaxed);
  Engine& engine = engine_for(order->symbol);

  if (order->is_cancel) {
    engine.cancel(order->id);
    counters_.cancels.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  const ExecutionReport report = engine.submit(order->to_order());
  const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();
  latency_.observe(std::chrono::duration<double>(finished - started).count());
  counters_.submits.fetch_add(1, std::memory_order_relaxed);

  for (const Trade& trade : report.trades) {
    TradeMessage published;
    published.symbol = order->symbol;
    published.taker_id = trade.taker_id;
    published.maker_id = trade.maker_id;
    published.price = trade.price;
    published.quantity = trade.quantity;
    const std::string encoded = encode(published);

    RdKafka::ErrorCode produced = RdKafka::ERR__QUEUE_FULL;
    while (produced == RdKafka::ERR__QUEUE_FULL) {
      produced = producer_->produce(
          config_.trades_topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
          const_cast<char*>(encoded.data()), encoded.size(), order->symbol.data(),
          order->symbol.size(), 0, nullptr);
      if (produced == RdKafka::ERR__QUEUE_FULL) {
        producer_->poll(100);
      }
    }
    if (produced != RdKafka::ERR_NO_ERROR) {
      std::fprintf(stderr, "produce failed: %s\n", RdKafka::err2str(produced).c_str());
      continue;
    }
    counters_.trades_published.fetch_add(1, std::memory_order_relaxed);
  }
}

// The ordering here is the durability argument: every outstanding trade is flushed and
// acknowledged before the consumer offset moves past the orders that produced them. A
// crash before this point replays those orders rather than losing their trades - which is
// at-least-once, so a replay can publish a trade twice.
bool EngineService::commit_progress(std::string& error) {
  const RdKafka::ErrorCode flushed = producer_->flush(10'000);
  if (flushed != RdKafka::ERR_NO_ERROR) {
    error = "flush: " + RdKafka::err2str(flushed);
    return false;
  }
  if (delivery_report().failures() > 0) {
    error = "delivery failures reported";
    return false;
  }

  const RdKafka::ErrorCode committed = consumer_->commitSync();
  if (committed != RdKafka::ERR_NO_ERROR && committed != RdKafka::ERR__NO_OFFSET) {
    error = "commit: " + RdKafka::err2str(committed);
    return false;
  }
  since_commit_ = 0;
  return true;
}

void EngineService::run(const std::atomic<bool>& running) {
  while (running.load()) {
    std::unique_ptr<RdKafka::Message> message(consumer_->consume(config_.poll_timeout_ms));

    switch (message->err()) {
      case RdKafka::ERR_NO_ERROR:
        handle(*message);
        ++since_commit_;
        break;
      case RdKafka::ERR__TIMED_OUT:
      case RdKafka::ERR__PARTITION_EOF:
        // Idle: commit whatever is outstanding so a quiet service does not sit on
        // uncommitted progress indefinitely.
        since_commit_ = since_commit_ > 0 ? config_.commit_interval : 0;
        break;
      default:
        std::fprintf(stderr, "consume error: %s\n", message->errstr().c_str());
        break;
    }

    producer_->poll(0);

    if (since_commit_ >= config_.commit_interval) {
      std::string error;
      if (!commit_progress(error)) {
        std::fprintf(stderr, "commit failed: %s\n", error.c_str());
      }
    }
  }

  std::string error;
  if (!commit_progress(error)) {
    std::fprintf(stderr, "final commit failed: %s\n", error.c_str());
  }
}

void EngineService::stop() {
  ready_ = false;
  if (producer_ != nullptr) {
    producer_->flush(5'000);
  }
  if (consumer_ != nullptr) {
    consumer_->close();
  }
}

}  // namespace matchbox::service
