#include "matching/engine.hpp"

#include <cassert>
#include <utility>

namespace matchbox {

Engine::Engine(std::unique_ptr<MatchingStrategy> strategy) : strategy_(std::move(strategy)) {
  assert(strategy_ != nullptr);
}

ExecutionReport Engine::submit(Order order) {
  assert(order.quantity > 0);
  assert(!book_.contains(order.id));

  order.sequence = next_sequence_++;

  const Quantity submitted = order.quantity;
  ExecutionReport report;
  report.order_id = order.id;
  strategy_->match(book_, order, report.trades);
  report.filled_quantity = submitted - order.quantity;

  if (order.quantity > 0 && order.type == OrderType::Limit) {
    book_.insert(order);
    report.resting_quantity = order.quantity;
  } else {
    // A market order never rests: whatever liquidity did not exist is simply cancelled.
    report.cancelled_quantity = order.quantity;
  }

  return report;
}

bool Engine::cancel(OrderId id) { return book_.cancel(id); }

}  // namespace matchbox
