#pragma once

#include <cstddef>
#include <list>

#include "orderbook/types.hpp"

namespace matchbox {

class OrderBook;

// Orders resting at a single price, held in arrival order: front() is the oldest and
// therefore the highest-priority order under price-time priority.
class PriceLevel {
 public:
  using OrderList = std::list<Order>;
  using const_iterator = OrderList::const_iterator;

  explicit PriceLevel(Price price) : price_(price) {}

  Price price() const { return price_; }
  Quantity total_quantity() const { return total_quantity_; }
  std::size_t order_count() const { return orders_.size(); }
  bool empty() const { return orders_.empty(); }
  const Order& front() const { return orders_.front(); }

  const_iterator begin() const { return orders_.begin(); }
  const_iterator end() const { return orders_.end(); }

 private:
  // Mutation is restricted to OrderBook because total_quantity_ and the book's order-id
  // index must be updated together; a caller holding a raw level could desynchronise them.
  friend class OrderBook;

  using iterator = OrderList::iterator;

  iterator append(const Order& order);
  void reduce(iterator position, Quantity quantity);
  void erase(iterator position);

  Price price_;
  Quantity total_quantity_{0};
  OrderList orders_;
};

}  // namespace matchbox
