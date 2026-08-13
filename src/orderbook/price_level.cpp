#include "orderbook/price_level.hpp"

#include <cassert>
#include <iterator>

namespace matchbox {

PriceLevel::iterator PriceLevel::append(const Order& order) {
  assert(order.price == price_);
  total_quantity_ += order.quantity;
  orders_.push_back(order);
  return std::prev(orders_.end());
}

void PriceLevel::reduce(iterator position, Quantity quantity) {
  assert(quantity < position->quantity);
  position->quantity -= quantity;
  total_quantity_ -= quantity;
}

void PriceLevel::erase(iterator position) {
  total_quantity_ -= position->quantity;
  orders_.erase(position);
}

}  // namespace matchbox
