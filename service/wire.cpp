#include "wire.hpp"

#include <nlohmann/json.hpp>

namespace matchbox::service {
namespace {

using nlohmann::json;

const char* to_string(Side side) { return side == Side::Buy ? "buy" : "sell"; }
const char* to_string(OrderType type) { return type == OrderType::Limit ? "limit" : "market"; }

std::optional<Side> side_from(const std::string& text) {
  if (text == "buy") {
    return Side::Buy;
  }
  if (text == "sell") {
    return Side::Sell;
  }
  return std::nullopt;
}

std::optional<OrderType> type_from(const std::string& text) {
  if (text == "limit") {
    return OrderType::Limit;
  }
  if (text == "market") {
    return OrderType::Market;
  }
  return std::nullopt;
}

}  // namespace

Order OrderMessage::to_order() const {
  Order order;
  order.id = id;
  order.side = side;
  order.type = type;
  order.price = type == OrderType::Market ? 0 : price;
  order.quantity = quantity;
  return order;
}

std::string encode(const OrderMessage& message) {
  json document;
  document["symbol"] = message.symbol;
  document["action"] = message.is_cancel ? "cancel" : "submit";
  document["id"] = message.id;
  if (!message.is_cancel) {
    document["side"] = to_string(message.side);
    document["type"] = to_string(message.type);
    document["quantity"] = message.quantity;
    if (message.type == OrderType::Limit) {
      document["price"] = message.price;
    }
  }
  return document.dump();
}

std::string encode(const TradeMessage& message) {
  json document;
  document["symbol"] = message.symbol;
  document["taker_id"] = message.taker_id;
  document["maker_id"] = message.maker_id;
  document["price"] = message.price;
  document["quantity"] = message.quantity;
  return document.dump();
}

std::optional<OrderMessage> decode_order(std::string_view json_text) {
  const json document = json::parse(json_text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  if (!document.contains("symbol") || !document.contains("action") || !document.contains("id")) {
    return std::nullopt;
  }

  OrderMessage message;
  message.symbol = document["symbol"].get<std::string>();
  const std::string action = document["action"].get<std::string>();
  if (action != "submit" && action != "cancel") {
    return std::nullopt;
  }
  message.is_cancel = action == "cancel";
  message.id = document["id"].get<OrderId>();

  if (message.is_cancel) {
    return message;
  }

  if (!document.contains("side") || !document.contains("type") ||
      !document.contains("quantity")) {
    return std::nullopt;
  }
  const std::optional<Side> side = side_from(document["side"].get<std::string>());
  const std::optional<OrderType> type = type_from(document["type"].get<std::string>());
  if (!side.has_value() || !type.has_value()) {
    return std::nullopt;
  }
  message.side = *side;
  message.type = *type;
  message.quantity = document["quantity"].get<Quantity>();
  if (message.quantity == 0) {
    return std::nullopt;
  }
  // A market order carries no price, so its absence is expected rather than malformed.
  if (message.type == OrderType::Limit) {
    if (!document.contains("price")) {
      return std::nullopt;
    }
    message.price = document["price"].get<Price>();
  }
  return message;
}

std::optional<TradeMessage> decode_trade(std::string_view json_text) {
  const json document = json::parse(json_text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  for (const char* field : {"symbol", "taker_id", "maker_id", "price", "quantity"}) {
    if (!document.contains(field)) {
      return std::nullopt;
    }
  }

  TradeMessage message;
  message.symbol = document["symbol"].get<std::string>();
  message.taker_id = document["taker_id"].get<OrderId>();
  message.maker_id = document["maker_id"].get<OrderId>();
  message.price = document["price"].get<Price>();
  message.quantity = document["quantity"].get<Quantity>();
  return message;
}

}  // namespace matchbox::service
