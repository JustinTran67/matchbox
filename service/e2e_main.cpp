// End-to-end check of the deployed system against an in-process ground truth.
//
// The same recorded order stream is (a) published to Kafka for the cluster to match and
// (b) replayed through a plain Engine here. The cluster's published trades then have to
// agree with the local replay, symbol by symbol, in order. The ground truth deliberately
// reuses the Phase 1-3 engine untouched, so this compares the distributed system against
// the thing already proven correct rather than against itself.

#include <librdkafka/rdkafkacpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "generator/market_simulator.hpp"
#include "matching/engine.hpp"
#include "matching/fifo_matcher.hpp"
#include "wire.hpp"

using namespace matchbox;
using namespace matchbox::service;
using Clock = std::chrono::steady_clock;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::string(value);
}

int env_int(const char* name, int fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::atoi(value);
}

struct SymbolPlan {
  std::string symbol;
  std::vector<OrderMessage> orders;
  std::vector<TradeMessage> expected_trades;
};

// Records a stream and its ground-truth trades in one pass: the generator adapts to the
// book it is fed, so the stream can only be replayed faithfully if it was captured against
// a book evolving exactly as the service's will.
SymbolPlan build_plan(const std::string& symbol, std::uint64_t seed, std::size_t steps) {
  SymbolPlan plan;
  plan.symbol = symbol;

  Engine engine(std::make_unique<FifoMatcher>());
  MarketSimulatorConfig config;
  MarketSimulator market(config, seed);

  for (std::size_t step = 0; step < steps; ++step) {
    const OrderGenerator::Action action = market.next(engine.book());

    OrderMessage message;
    message.symbol = symbol;
    if (action.kind == OrderGenerator::Action::Kind::Cancel) {
      message.is_cancel = true;
      message.id = action.cancel_id;
      plan.orders.push_back(message);
      engine.cancel(action.cancel_id);
      continue;
    }

    const Order& order = action.order;
    message.is_cancel = false;
    message.id = order.id;
    message.side = order.side;
    message.type = order.type;
    message.price = order.price;
    message.quantity = order.quantity;
    plan.orders.push_back(message);

    const ExecutionReport report = engine.submit(order);
    for (const Trade& trade : report.trades) {
      plan.expected_trades.push_back(
          TradeMessage{symbol, trade.taker_id, trade.maker_id, trade.price, trade.quantity});
    }
  }
  return plan;
}

// Publishes one recorded stream, shifting every id by `offset`. Load rounds must not
// replay the same ids: an id already resting on a book would collide in the engine's
// order-id index, which is a corruption the demo would then be measuring.
// `paced_from`/`target_rate` throttle inside the loop rather than between calls. Pacing a
// whole round at a time would publish every order at line rate and then sleep off the
// remainder, which arrives as a burst followed by silence instead of a steady stream.
std::size_t publish_stream(RdKafka::Producer& producer, const std::string& topic,
                           const std::vector<SymbolPlan>& plans, OrderId offset,
                           double target_rate, Clock::time_point paced_from,
                           std::size_t already_published) {
  std::size_t published = 0;

  // Round-robin across symbols rather than draining one at a time. Symbols map onto
  // partitions, and partitions onto engines, so publishing a symbol to exhaustion leaves
  // three of the four engines idle for the duration of that block.
  std::size_t longest = 0;
  for (const SymbolPlan& plan : plans) {
    longest = std::max(longest, plan.orders.size());
  }

  for (std::size_t index = 0; index < longest; ++index) {
    for (const SymbolPlan& plan : plans) {
      if (index >= plan.orders.size()) {
        continue;
      }
      const OrderMessage& original = plan.orders[index];
      OrderMessage message = original;
      message.id += offset;
      const std::string payload = encode(message);

      RdKafka::ErrorCode produced = RdKafka::ERR__QUEUE_FULL;
      while (produced == RdKafka::ERR__QUEUE_FULL) {
        // Keyed by symbol, with no explicit partition: the partitioner decides, which is
        // what guarantees every order for a symbol lands on one partition in order.
        produced = producer.produce(topic, RdKafka::Topic::PARTITION_UA,
                                    RdKafka::Producer::RK_MSG_COPY,
                                    const_cast<char*>(payload.data()), payload.size(),
                                    plan.symbol.data(), plan.symbol.size(), 0, nullptr);
        if (produced == RdKafka::ERR__QUEUE_FULL) {
          producer.poll(100);
        }
      }
      if (produced == RdKafka::ERR_NO_ERROR) {
        ++published;
      }

      // Checked every 25 orders so the sleep granularity stays coarse enough not to
      // dominate the work being paced.
      if (target_rate > 0.0 && published % 25 == 0) {
        const double owed = static_cast<double>(already_published + published) / target_rate;
        const double spent = std::chrono::duration<double>(Clock::now() - paced_from).count();
        if (owed > spent) {
          std::this_thread::sleep_for(std::chrono::duration<double>(owed - spent));
        }
      }
    }
  }
  return published;
}

bool same(const TradeMessage& a, const TradeMessage& b) {
  return a.symbol == b.symbol && a.taker_id == b.taker_id && a.maker_id == b.maker_id &&
         a.price == b.price && a.quantity == b.quantity;
}

}  // namespace

int main() {
  const std::string brokers = env_or("KAFKA_BROKERS", "localhost:9092");
  const std::string orders_topic = env_or("ORDERS_TOPIC", "matchbox.orders");
  const std::string trades_topic = env_or("TRADES_TOPIC", "matchbox.trades");
  const std::size_t steps = static_cast<std::size_t>(env_int("STEPS", 5'000));
  const int wait_seconds = env_int("WAIT_SECONDS", 60);

  const std::vector<std::string> symbols = {"AAPL", "MSFT", "AMZN", "META"};

  std::printf("matchbox end-to-end verification\n");
  std::printf("  brokers : %s\n", brokers.c_str());
  std::printf("  steps   : %zu per symbol across %zu symbols\n", steps, symbols.size());

  std::vector<SymbolPlan> plans;
  std::size_t expected_total = 0;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    plans.push_back(build_plan(symbols[i], 1'000 + i * 31, steps));
    expected_total += plans.back().expected_trades.size();
    std::printf("  %-6s %zu orders -> %zu expected trades\n", plans.back().symbol.c_str(),
                plans.back().orders.size(), plans.back().expected_trades.size());
  }
  if (expected_total == 0) {
    std::fprintf(stderr, "generated stream produced no trades; nothing to verify\n");
    return 1;
  }

  std::string error;
  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  conf->set("bootstrap.servers", brokers, error);

  std::unique_ptr<RdKafka::Producer> producer(RdKafka::Producer::create(conf.get(), error));
  if (producer == nullptr) {
    std::fprintf(stderr, "producer: %s\n", error.c_str());
    return 1;
  }

  // Traffic generator for dashboards. Verification is skipped deliberately: the ground
  // truth here is a replay against an empty book, so comparing it against a cluster whose
  // books already hold state reports a divergence that says nothing about correctness.
  if (env_int("LOAD_ONLY", 0) != 0) {
    const int load_seconds = env_int("LOAD_SECONDS", 120);
    const double target_rate = static_cast<double>(env_int("LOAD_ORDERS_PER_SEC", 2'000));
    constexpr OrderId kRoundStride = 10'000'000;

    // Rate limited on purpose. Unthrottled, librdkafka accepts orders far faster than the
    // engines drain them, so consumer lag grows without bound and the books swell until a
    // pod hits its memory limit - which produces a crash loop rather than a demo. Pacing
    // below the engines' drain rate keeps the books near steady state.
    std::printf("\nload-only: publishing for %ds at ~%.0f orders/sec (no verification)\n",
                load_seconds, target_rate);
    const Clock::time_point began = Clock::now();
    const Clock::time_point until = began + std::chrono::seconds(load_seconds);

    std::size_t total = 0;
    OrderId round = 0;
    while (Clock::now() < until) {
      total += publish_stream(*producer, orders_topic, plans, round * kRoundStride,
                              target_rate, began, total);
      producer->flush(30'000);
      ++round;

      std::printf("  round %llu: %zu orders published (%.0f/sec)\n",
                  static_cast<unsigned long long>(round), total,
                  static_cast<double>(total) /
                      std::chrono::duration<double>(Clock::now() - began).count());
      std::fflush(stdout);
    }

    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    std::printf("\npublished %zu orders over %.1fs (%.0f orders/sec offered)\n", total, elapsed,
                static_cast<double>(total) / elapsed);
    return 0;
  }

  // The consumer is started before publishing so no trade can be produced and expired
  // before anything is listening for it.
  std::unique_ptr<RdKafka::Conf> consumer_conf(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  consumer_conf->set("bootstrap.servers", brokers, error);
  consumer_conf->set("group.id", env_or("E2E_GROUP", "matchbox-e2e"), error);
  consumer_conf->set("auto.offset.reset", "earliest", error);
  consumer_conf->set("enable.auto.commit", "false", error);

  std::unique_ptr<RdKafka::KafkaConsumer> consumer(
      RdKafka::KafkaConsumer::create(consumer_conf.get(), error));
  if (consumer == nullptr) {
    std::fprintf(stderr, "consumer: %s\n", error.c_str());
    return 1;
  }
  if (consumer->subscribe({trades_topic}) != RdKafka::ERR_NO_ERROR) {
    std::fprintf(stderr, "subscribe to %s failed\n", trades_topic.c_str());
    return 1;
  }

  const std::size_t published = publish_stream(*producer, orders_topic, plans, 0, 0.0, Clock::now(), 0);
  producer->flush(30'000);
  std::printf("published %zu orders, expecting %zu trades\n", published, expected_total);
  std::fflush(stdout);

  std::map<std::string, std::vector<TradeMessage>> observed;
  std::size_t received = 0;
  std::size_t malformed = 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(wait_seconds);
  Clock::time_point last_progress = Clock::now();

  while (received < expected_total && Clock::now() < deadline) {
    std::unique_ptr<RdKafka::Message> message(consumer->consume(1'000));
    if (message->err() == RdKafka::ERR_NO_ERROR) {
      const std::string_view payload(static_cast<const char*>(message->payload()),
                                     message->len());
      const std::optional<TradeMessage> trade = decode_trade(payload);
      if (!trade.has_value()) {
        ++malformed;
        continue;
      }
      observed[trade->symbol].push_back(*trade);
      ++received;
      last_progress = Clock::now();
      if (received % 5'000 == 0) {
        std::printf("  received %zu/%zu\n", received, expected_total);
        std::fflush(stdout);
      }
    } else if (message->err() != RdKafka::ERR__TIMED_OUT &&
               message->err() != RdKafka::ERR__PARTITION_EOF) {
      std::fprintf(stderr, "consume error: %s\n", message->errstr().c_str());
    }

    // Stop early once the flow has clearly dried up, rather than always burning the full
    // timeout when trades are missing.
    if (received > 0 && Clock::now() - last_progress > std::chrono::seconds(15)) {
      std::fprintf(stderr, "no new trades for 15s; stopping early\n");
      break;
    }
  }
  consumer->close();

  std::printf("\nreceived %zu trades (%zu malformed)\n", received, malformed);
  std::printf("%-8s %10s %10s   %s\n", "symbol", "expected", "observed", "result");
  std::printf("--------------------------------------------------\n");

  bool ok = received == expected_total && malformed == 0;
  for (const SymbolPlan& plan : plans) {
    const std::vector<TradeMessage>& actual = observed[plan.symbol];
    bool matches = actual.size() == plan.expected_trades.size();
    std::size_t first_divergence = 0;
    if (matches) {
      for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!same(actual[i], plan.expected_trades[i])) {
          matches = false;
          first_divergence = i;
          break;
        }
      }
    }
    std::printf("%-8s %10zu %10zu   %s", plan.symbol.c_str(), plan.expected_trades.size(),
                actual.size(), matches ? "MATCH\n" : "DIVERGED");
    if (!matches && actual.size() == plan.expected_trades.size()) {
      std::printf(" at trade %zu\n", first_divergence);
    } else if (!matches) {
      std::printf(" (count mismatch)\n");
    }
    ok = ok && matches;
  }

  std::printf("\n%s\n", ok ? "PASS: cluster trades match in-process ground truth"
                           : "FAIL: cluster trades diverge from ground truth");
  return ok ? 0 : 1;
}
