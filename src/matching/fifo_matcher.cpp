#include "matching/fifo_matcher.hpp"

#include <algorithm>

namespace matchbox {

void FifoMatcher::match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) {
  const Side resting_side = opposite(incoming.side);

  while (incoming.quantity > 0) {
    const PriceLevel* level = book.best_level(resting_side);
    if (level == nullptr || !crosses(incoming, level->price())) {
      break;
    }

    // Both the level and its front order can be destroyed by the fill below, so everything
    // needed from them is copied out first and re-read from the book on the next pass.
    const Order& maker = level->front();
    const Quantity quantity = std::min(incoming.quantity, maker.quantity);
    const OrderId maker_id = maker.id;

    trades.push_back(Trade{incoming.id, maker_id, level->price(), quantity});
    incoming.quantity -= quantity;
    book.fill(maker_id, quantity);
  }
}

}  // namespace matchbox
