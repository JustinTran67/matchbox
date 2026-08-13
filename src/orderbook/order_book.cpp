#include "orderbook/order_book.hpp"

#include <cassert>
#include <utility>

namespace matchbox {
namespace {

template <typename Levels>
PriceLevel& level_for(Levels& levels, Price price) {
  return levels.try_emplace(price, price).first->second;
}

template <typename Levels>
Quantity depth_of(const Levels& levels, Price price) {
  const auto level = levels.find(price);
  return level == levels.end() ? 0 : level->second.total_quantity();
}

}  // namespace

template <typename Levels>
void OrderBook::erase_order(Levels& levels, Price price, PriceLevel::OrderList::iterator position) {
  const auto level = levels.find(price);
  assert(level != levels.end());
  level->second.erase(position);
  if (level->second.empty()) {
    levels.erase(level);
  }
}

void OrderBook::insert(const Order& order) {
  assert(order.type == OrderType::Limit);
  assert(order.quantity > 0);
  assert(!contains(order.id));

  PriceLevel& level =
      order.side == Side::Buy ? level_for(bids_, order.price) : level_for(asks_, order.price);
  locators_.emplace(order.id, Locator{order.side, order.price, level.append(order)});
}

bool OrderBook::cancel(OrderId id) {
  const auto locator = locators_.find(id);
  if (locator == locators_.end()) {
    return false;
  }
  remove(locator);
  return true;
}

void OrderBook::fill(OrderId id, Quantity quantity) {
  const auto locator = locators_.find(id);
  assert(locator != locators_.end());
  assert(quantity > 0 && quantity <= locator->second.position->quantity);

  if (quantity == locator->second.position->quantity) {
    remove(locator);
    return;
  }

  const Locator& location = locator->second;
  if (location.side == Side::Buy) {
    bids_.find(location.price)->second.reduce(location.position, quantity);
  } else {
    asks_.find(location.price)->second.reduce(location.position, quantity);
  }
}

void OrderBook::remove(LocatorMap::iterator locator) {
  const Locator& location = locator->second;
  if (location.side == Side::Buy) {
    erase_order(bids_, location.price, location.position);
  } else {
    erase_order(asks_, location.price, location.position);
  }
  locators_.erase(locator);
}

const PriceLevel* OrderBook::best_level(Side side) const {
  if (side == Side::Buy) {
    return bids_.empty() ? nullptr : &bids_.begin()->second;
  }
  return asks_.empty() ? nullptr : &asks_.begin()->second;
}

PriceLevel* OrderBook::best_level(Side side) {
  return const_cast<PriceLevel*>(std::as_const(*this).best_level(side));
}

std::optional<Price> OrderBook::best_price(Side side) const {
  const PriceLevel* level = best_level(side);
  return level == nullptr ? std::nullopt : std::optional<Price>(level->price());
}

Quantity OrderBook::depth_at(Side side, Price price) const {
  return side == Side::Buy ? depth_of(bids_, price) : depth_of(asks_, price);
}

const Order* OrderBook::find(OrderId id) const {
  const auto locator = locators_.find(id);
  return locator == locators_.end() ? nullptr : &*locator->second.position;
}

std::size_t OrderBook::level_count(Side side) const {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

bool OrderBook::crossed() const {
  const auto bid = best_price(Side::Buy);
  const auto ask = best_price(Side::Sell);
  return bid.has_value() && ask.has_value() && *bid >= *ask;
}

}  // namespace matchbox
