#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace matchbox::testing {

// An independent model of what the book should contain, built only from execution
// reports. It deliberately shares no code with OrderBook: checking the engine against its
// own front() would let a bug in the book's queue ordering satisfy the matcher that reads
// it. Cancelled and filled orders are pruned lazily when a queue is inspected.
class BookModel {
 public:
  struct RestingOrder {
    OrderId id;
    Quantity quantity;
  };

  // A whole price level, oldest order first - what a pro-rata allocation is computed from.
  struct LevelView {
    Price price;
    std::vector<RestingOrder> orders;
  };

  void rest(OrderId id, Side side, Price price, Quantity quantity) {
    live_.emplace(id, Entry{quantity, price, side});
    (side == Side::Buy ? bids_[price] : asks_[price]).push_back(id);
  }

  bool live(OrderId id) const { return live_.count(id) != 0; }

  void drop(OrderId id) { live_.erase(id); }

  Quantity quantity_of(OrderId id) const {
    const auto entry = live_.find(id);
    return entry == live_.end() ? 0 : entry->second.quantity;
  }

  void reduce(OrderId id, Quantity quantity) {
    const auto entry = live_.find(id);
    entry->second.quantity -= quantity;
    if (entry->second.quantity == 0) {
      live_.erase(entry);
    }
  }

  // The order that price-time priority requires the next trade on `side` to hit.
  std::optional<std::pair<Price, OrderId>> best_resting(Side side) {
    return side == Side::Buy ? best_of(bids_) : best_of(asks_);
  }

  std::optional<LevelView> best_level(Side side) {
    return side == Side::Buy ? best_level_of(bids_) : best_level_of(asks_);
  }

  std::size_t size() const { return live_.size(); }

  bool agrees_with(const OrderBook& book) const {
    if (book.resting_order_count() != live_.size()) {
      return false;
    }
    for (const auto& [id, entry] : live_) {
      const Order* order = book.find(id);
      if (order == nullptr || order->quantity != entry.quantity || order->price != entry.price ||
          order->side != entry.side) {
        return false;
      }
    }
    return true;
  }

 private:
  struct Entry {
    Quantity quantity;
    Price price;
    Side side;
  };

  template <typename Levels>
  std::optional<std::pair<Price, OrderId>> best_of(Levels& levels) {
    while (!levels.empty()) {
      auto level = levels.begin();
      std::deque<OrderId>& queue = level->second;
      while (!queue.empty() && live_.count(queue.front()) == 0) {
        queue.pop_front();
      }
      if (queue.empty()) {
        levels.erase(level);
        continue;
      }
      return std::make_pair(level->first, queue.front());
    }
    return std::nullopt;
  }

  template <typename Levels>
  std::optional<LevelView> best_level_of(Levels& levels) {
    while (!levels.empty()) {
      auto level = levels.begin();
      std::deque<OrderId>& queue = level->second;
      queue.erase(std::remove_if(queue.begin(), queue.end(),
                                 [this](OrderId id) { return live_.count(id) == 0; }),
                  queue.end());
      if (queue.empty()) {
        levels.erase(level);
        continue;
      }

      LevelView view;
      view.price = level->first;
      view.orders.reserve(queue.size());
      for (const OrderId id : queue) {
        view.orders.push_back(RestingOrder{id, live_.at(id).quantity});
      }
      return view;
    }
    return std::nullopt;
  }

  std::unordered_map<OrderId, Entry> live_;
  std::map<Price, std::deque<OrderId>, std::greater<Price>> bids_;
  std::map<Price, std::deque<OrderId>, std::less<Price>> asks_;
};

}  // namespace matchbox::testing
