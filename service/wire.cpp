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

// nlohmann's parse(..., allow_exceptions=false) suppresses *parse* errors only; get<T>()
// on a mistyped field still throws. Untrusted input reaches this decoder straight off a
// Kafka topic, and an escaping exception terminates the process - and because the offset
// is not committed past the message, the pod crash-loops on the same poison pill forever.
// Every field is therefore type-checked before extraction.
bool read_id(const json& document, const char* key, OrderId& out) {
  if (!document.contains(key) || !document[key].is_number_unsigned()) {
    return false;
  }
  out = document[key].get<OrderId>();
  return true;
}

bool read_quantity(const json& document, const char* key, Quantity& out) {
  if (!document.contains(key) || !document[key].is_number_unsigned()) {
    return false;
  }
  out = document[key].get<Quantity>();
  return true;
}

bool read_price(const json& document, const char* key, Price& out) {
  if (!document.contains(key) || !document[key].is_number_integer()) {
    return false;
  }
  out = document[key].get<Price>();
  return true;
}

bool read_string(const json& document, const char* key, std::string& out) {
  if (!document.contains(key) || !document[key].is_string()) {
    return false;
  }
  out = document[key].get<std::string>();
  return true;
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

  OrderMessage message;
  std::string action;
  if (!read_string(document, "symbol", message.symbol) ||
      !read_string(document, "action", action) || !read_id(document, "id", message.id)) {
    return std::nullopt;
  }
  if (message.symbol.empty() || (action != "submit" && action != "cancel")) {
    return std::nullopt;
  }
  message.is_cancel = action == "cancel";
  if (message.is_cancel) {
    return message;
  }

  std::string side_text;
  std::string type_text;
  if (!read_string(document, "side", side_text) || !read_string(document, "type", type_text) ||
      !read_quantity(document, "quantity", message.quantity)) {
    return std::nullopt;
  }

  const std::optional<Side> side = side_from(side_text);
  const std::optional<OrderType> type = type_from(type_text);
  if (!side.has_value() || !type.has_value() || message.quantity == 0) {
    return std::nullopt;
  }
  message.side = *side;
  message.type = *type;

  // A market order carries no price, so its absence is expected rather than malformed.
  if (message.type == OrderType::Limit && !read_price(document, "price", message.price)) {
    return std::nullopt;
  }
  return message;
}

std::optional<TradeMessage> decode_trade(std::string_view json_text) {
  const json document = json::parse(json_text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }

  TradeMessage message;
  if (!read_string(document, "symbol", message.symbol) ||
      !read_id(document, "taker_id", message.taker_id) ||
      !read_id(document, "maker_id", message.maker_id) ||
      !read_price(document, "price", message.price) ||
      !read_quantity(document, "quantity", message.quantity)) {
    return std::nullopt;
  }
  return message;
}

}  // namespace matchbox::service
