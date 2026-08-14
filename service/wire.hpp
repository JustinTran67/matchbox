#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "orderbook/types.hpp"

namespace matchbox::service {

// One action on one symbol's book, as it travels over Kafka. Deliberately a separate type
// from `Order`: the engine's order carries no symbol, because routing is the transport's
// job and the engine is symbol-agnostic by design.
struct OrderMessage {
  std::string symbol;
  bool is_cancel{false};
  OrderId id{0};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  Price price{0};
  Quantity quantity{0};

  Order to_order() const;
};

struct TradeMessage {
  std::string symbol;
  OrderId taker_id{0};
  OrderId maker_id{0};
  Price price{0};
  Quantity quantity{0};
};

std::string encode(const OrderMessage& message);
std::string encode(const TradeMessage& message);

// Return nothing rather than throwing on malformed input: a bad message on the wire is an
// expected condition for a service, not an exceptional one.
std::optional<OrderMessage> decode_order(std::string_view json);
std::optional<TradeMessage> decode_trade(std::string_view json);

}  // namespace matchbox::service
