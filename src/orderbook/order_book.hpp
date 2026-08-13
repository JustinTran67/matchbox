#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "orderbook/price_level.hpp"
#include "orderbook/types.hpp"

namespace matchbox {

// Resting orders for a single symbol. Bids and asks are each kept in a price-ordered map
// whose first element is the best price, giving O(1) top-of-book access and O(log levels)
// insertion, plus an order-id index that makes cancel and fill O(1) amortised.
class OrderBook {
 public:
  void insert(const Order& order);
  bool cancel(OrderId id);
  void fill(OrderId id, Quantity quantity);

  PriceLevel* best_level(Side side);
  const PriceLevel* best_level(Side side) const;
  std::optional<Price> best_price(Side side) const;
  Quantity depth_at(Side side, Price price) const;

  const Order* find(OrderId id) const;
  bool contains(OrderId id) const { return find(id) != nullptr; }
  std::size_t resting_order_count() const { return locators_.size(); }
  std::size_t level_count(Side side) const;
  bool crossed() const;

 private:
  struct Locator {
    Side side;
    Price price;
    PriceLevel::OrderList::iterator position;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;
  using LocatorMap = std::unordered_map<OrderId, Locator>;

  template <typename Levels>
  static void erase_order(Levels& levels, Price price, PriceLevel::OrderList::iterator position);

  void remove(LocatorMap::iterator locator);

  BidLevels bids_;
  AskLevels asks_;
  LocatorMap locators_;
};

}  // namespace matchbox
