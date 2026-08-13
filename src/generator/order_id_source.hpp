#pragma once

#include "orderbook/types.hpp"

namespace matchbox {

// A single monotonic id counter for one symbol. Trader populations that share a book must
// share one of these: ids are the engine's identity for an order, so two populations
// counting independently would hand the book duplicate ids.
class OrderIdSource {
 public:
  explicit OrderIdSource(OrderId first = 1) : next_(first) {}

  OrderId take() { return next_++; }
  OrderId peek() const { return next_; }

 private:
  OrderId next_;
};

}  // namespace matchbox
