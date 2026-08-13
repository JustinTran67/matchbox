#include "matching/pro_rata_matcher.hpp"

#include <cassert>

namespace matchbox {

void ProRataMatcher::match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) {
  const Side resting_side = opposite(incoming.side);

  while (incoming.quantity > 0) {
    const PriceLevel* level = book.best_level(resting_side);
    if (level == nullptr || !crosses(incoming, level->price())) {
      break;
    }

    // Allocation has to be decided before anything is filled: a fill can erase orders and
    // the level itself, so the level is snapshotted and never read again after that.
    const Price price = level->price();
    const Quantity level_total = level->total_quantity();

    scratch_.clear();
    for (const Order& resting : *level) {
      scratch_.push_back(Allocation{resting.id, resting.quantity, 0});
    }

    const Quantity taken = allocate(incoming.quantity, level_total);
    assert(taken > 0);
    assert(taken <= incoming.quantity);

    for (const Allocation& allocation : scratch_) {
      if (allocation.allocated == 0) {
        continue;
      }
      trades.push_back(Trade{incoming.id, allocation.id, price, allocation.allocated});
      book.fill(allocation.id, allocation.allocated);
    }

    incoming.quantity -= taken;

    // Either the level was cleared or the incoming order was exhausted, so the next pass
    // always sees a strictly smaller problem and the loop cannot spin.
    assert(taken == level_total || incoming.quantity == 0);
  }
}

Quantity ProRataMatcher::allocate(Quantity incoming_quantity, Quantity level_total) {
  assert(level_total > 0);

  if (incoming_quantity >= level_total) {
    for (Allocation& allocation : scratch_) {
      allocation.allocated = allocation.resting;
    }
    return level_total;
  }

  Quantity allocated_total = 0;
  for (Allocation& allocation : scratch_) {
    // Widened because the numerator is a product of two order quantities.
    allocation.allocated = static_cast<Quantity>(
        (static_cast<unsigned __int128>(incoming_quantity) * allocation.resting) / level_total);
    allocated_total += allocation.allocated;
  }

  // Flooring every share strands fewer units than there are orders, and each share is
  // strictly below that order's resting size because incoming_quantity < level_total.
  // So one unit each to the oldest orders places the whole remainder, and no order can be
  // pushed past what it actually has resting.
  for (Allocation& allocation : scratch_) {
    if (allocated_total == incoming_quantity) {
      break;
    }
    if (allocation.allocated < allocation.resting) {
      ++allocation.allocated;
      ++allocated_total;
    }
  }

  assert(allocated_total == incoming_quantity);
  return allocated_total;
}

}  // namespace matchbox
