#pragma once

#include <cstdint>

namespace matchbox {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using Sequence = std::uint64_t;

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

constexpr Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

struct Order {
  OrderId id{0};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  Price price{0};
  Quantity quantity{0};
  Sequence sequence{0};
};

struct Trade {
  OrderId taker_id{0};
  OrderId maker_id{0};
  Price price{0};
  Quantity quantity{0};
};

}  // namespace matchbox
