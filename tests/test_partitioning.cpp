#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "partition_assignment.hpp"
#include "wire.hpp"

using namespace matchbox;
using matchbox::service::ordinal_from_pod_name;
using matchbox::service::partitions_for;

TEST_CASE("every partition is owned by exactly one replica, for every replica count") {
  constexpr int kMaxPartitions = 24;

  for (int partition_count = 1; partition_count <= kMaxPartitions; ++partition_count) {
    for (int replica_count = 1; replica_count <= partition_count; ++replica_count) {
      std::vector<int> owner_count(static_cast<std::size_t>(partition_count), 0);

      for (int ordinal = 0; ordinal < replica_count; ++ordinal) {
        for (const std::int32_t partition : partitions_for(ordinal, replica_count, partition_count)) {
          REQUIRE(partition >= 0);
          REQUIRE(partition < partition_count);
          ++owner_count[static_cast<std::size_t>(partition)];
        }
      }

      for (int partition = 0; partition < partition_count; ++partition) {
        INFO("partitions=" << partition_count << " replicas=" << replica_count
                           << " partition=" << partition);
        // Not "at least one" and not "at most one": exactly one. A gap silently drops a
        // symbol's order flow; an overlap gives one book two owners.
        REQUIRE(owner_count[static_cast<std::size_t>(partition)] == 1);
      }
    }
  }
}

TEST_CASE("ownership is contiguous and balanced to within one partition") {
  constexpr int kPartitions = 16;

  for (int replica_count = 1; replica_count <= kPartitions; ++replica_count) {
    std::size_t smallest = kPartitions;
    std::size_t largest = 0;

    for (int ordinal = 0; ordinal < replica_count; ++ordinal) {
      const std::vector<std::int32_t> owned = partitions_for(ordinal, replica_count, kPartitions);
      REQUIRE_FALSE(owned.empty());
      REQUIRE(std::is_sorted(owned.begin(), owned.end()));
      REQUIRE(owned.back() - owned.front() == static_cast<std::int32_t>(owned.size()) - 1);

      smallest = std::min(smallest, owned.size());
      largest = std::max(largest, owned.size());
    }
    INFO("replicas=" << replica_count);
    REQUIRE(largest - smallest <= 1);
  }
}

TEST_CASE("more replicas than partitions leaves the surplus idle rather than doubling up") {
  constexpr int kPartitions = 4;
  constexpr int kReplicas = 7;

  std::vector<int> owner_count(kPartitions, 0);
  int idle = 0;
  for (int ordinal = 0; ordinal < kReplicas; ++ordinal) {
    const std::vector<std::int32_t> owned = partitions_for(ordinal, kReplicas, kPartitions);
    if (owned.empty()) {
      ++idle;
    }
    for (const std::int32_t partition : owned) {
      ++owner_count[static_cast<std::size_t>(partition)];
    }
  }

  REQUIRE(idle == kReplicas - kPartitions);
  for (int partition = 0; partition < kPartitions; ++partition) {
    REQUIRE(owner_count[static_cast<std::size_t>(partition)] == 1);
  }
}

TEST_CASE("an out-of-range or malformed ordinal owns nothing") {
  REQUIRE(partitions_for(-1, 4, 4).empty());
  REQUIRE(partitions_for(4, 4, 4).empty());
  REQUIRE(partitions_for(0, 0, 4).empty());
  REQUIRE(partitions_for(0, 4, 0).empty());
}

TEST_CASE("pod ordinals are read off the end of the StatefulSet name") {
  REQUIRE(ordinal_from_pod_name("matchbox-engine-0") == 0);
  REQUIRE(ordinal_from_pod_name("matchbox-engine-2") == 2);
  REQUIRE(ordinal_from_pod_name("matchbox-engine-13") == 13);
  REQUIRE(ordinal_from_pod_name("engine-0-7") == 7);

  REQUIRE_FALSE(ordinal_from_pod_name("matchbox-engine").has_value());
  REQUIRE_FALSE(ordinal_from_pod_name("").has_value());
  REQUIRE_FALSE(ordinal_from_pod_name("matchbox-engine-").has_value());
}

TEST_CASE("a limit order survives a round trip over the wire") {
  service::OrderMessage sent;
  sent.symbol = "AAPL";
  sent.id = 4242;
  sent.side = Side::Sell;
  sent.type = OrderType::Limit;
  sent.price = 10'150;
  sent.quantity = 250;

  const std::optional<service::OrderMessage> received = service::decode_order(encode(sent));
  REQUIRE(received.has_value());
  REQUIRE(received->symbol == "AAPL");
  REQUIRE(received->id == 4242);
  REQUIRE(received->side == Side::Sell);
  REQUIRE(received->type == OrderType::Limit);
  REQUIRE(received->price == 10'150);
  REQUIRE(received->quantity == 250);
  REQUIRE_FALSE(received->is_cancel);

  const Order order = received->to_order();
  REQUIRE(order.id == 4242);
  REQUIRE(order.price == 10'150);
}

TEST_CASE("a market order round trips without carrying a price") {
  service::OrderMessage sent;
  sent.symbol = "MSFT";
  sent.id = 7;
  sent.side = Side::Buy;
  sent.type = OrderType::Market;
  sent.quantity = 90;

  const std::string encoded = encode(sent);
  REQUIRE(encoded.find("price") == std::string::npos);

  const std::optional<service::OrderMessage> received = service::decode_order(encoded);
  REQUIRE(received.has_value());
  REQUIRE(received->type == OrderType::Market);
  REQUIRE(received->quantity == 90);
  REQUIRE(received->to_order().price == 0);
}

TEST_CASE("a cancel round trips carrying only its id") {
  service::OrderMessage sent;
  sent.symbol = "META";
  sent.is_cancel = true;
  sent.id = 99;

  const std::optional<service::OrderMessage> received = service::decode_order(encode(sent));
  REQUIRE(received.has_value());
  REQUIRE(received->is_cancel);
  REQUIRE(received->id == 99);
  REQUIRE(received->symbol == "META");
}

TEST_CASE("a trade round trips over the wire") {
  service::TradeMessage sent{"AMZN", 11, 22, 9'900, 40};

  const std::optional<service::TradeMessage> received = service::decode_trade(encode(sent));
  REQUIRE(received.has_value());
  REQUIRE(received->symbol == "AMZN");
  REQUIRE(received->taker_id == 11);
  REQUIRE(received->maker_id == 22);
  REQUIRE(received->price == 9'900);
  REQUIRE(received->quantity == 40);
}

TEST_CASE("malformed wire input is rejected rather than half-decoded") {
  REQUIRE_FALSE(service::decode_order("not json").has_value());
  REQUIRE_FALSE(service::decode_order("[]").has_value());
  REQUIRE_FALSE(service::decode_order(R"({"symbol":"A","action":"submit"})").has_value());
  REQUIRE_FALSE(service::decode_order(R"({"symbol":"A","action":"nope","id":1})").has_value());
  REQUIRE_FALSE(
      service::decode_order(
          R"({"symbol":"A","action":"submit","id":1,"side":"up","type":"limit","quantity":5,"price":1})")
          .has_value());
  // A zero-quantity order would trip an engine precondition, so it is refused at the edge.
  REQUIRE_FALSE(
      service::decode_order(
          R"({"symbol":"A","action":"submit","id":1,"side":"buy","type":"limit","quantity":0,"price":1})")
          .has_value());
  REQUIRE_FALSE(service::decode_trade(R"({"symbol":"A","taker_id":1})").has_value());
}

TEST_CASE("mistyped wire fields are rejected instead of throwing") {
  // parse(..., allow_exceptions=false) stops parse errors but not get<T>() type errors.
  // An exception escaping here terminates the service, and since the offset is not
  // committed past the message, the pod would crash-loop on the same one indefinitely.
  const char* poison[] = {
      R"({"symbol":"A","action":"submit","id":"nope","side":"buy","type":"limit","quantity":5,"price":1})",
      R"({"symbol":"A","action":"submit","id":-1,"side":"buy","type":"limit","quantity":5,"price":1})",
      R"({"symbol":"A","action":"submit","id":1,"side":"buy","type":"limit","quantity":-5,"price":1})",
      R"({"symbol":123,"action":"submit","id":1,"side":"buy","type":"limit","quantity":5,"price":1})",
      R"({"symbol":"A","action":"submit","id":1,"side":"buy","type":"limit","quantity":5,"price":"x"})",
      R"({"symbol":"A","action":"submit","id":1,"side":true,"type":"limit","quantity":5,"price":1})",
      R"({"symbol":"A","action":"cancel","id":{"nested":1}})",
      R"({"symbol":"","action":"submit","id":1,"side":"buy","type":"limit","quantity":5,"price":1})",
  };
  for (const char* payload : poison) {
    INFO(payload);
    REQUIRE_NOTHROW(service::decode_order(payload));
    REQUIRE_FALSE(service::decode_order(payload).has_value());
  }

  const char* poison_trades[] = {
      R"({"symbol":"A","taker_id":"x","maker_id":2,"price":1,"quantity":1})",
      R"({"symbol":"A","taker_id":1,"maker_id":2,"price":{},"quantity":1})",
      R"({"symbol":null,"taker_id":1,"maker_id":2,"price":1,"quantity":1})",
  };
  for (const char* payload : poison_trades) {
    INFO(payload);
    REQUIRE_NOTHROW(service::decode_trade(payload));
    REQUIRE_FALSE(service::decode_trade(payload).has_value());
  }
}
