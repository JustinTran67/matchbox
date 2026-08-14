#include "matching/engine.hpp"

#include <cassert>
#include <utility>

namespace matchbox {

Engine::Engine(std::unique_ptr<MatchingStrategy> strategy) : strategy_(std::move(strategy)) {
  assert(strategy_ != nullptr);
}

ExecutionReport Engine::submit(Order order) {
  ExecutionReport report;
  report.order_id = order.id;

  // Checked at runtime, not by assert: order ids arrive from outside the process, and
  // optimised builds define NDEBUG, so an assert here would vanish exactly where the
  // untrusted input actually lands.
  if (order.quantity == 0 || book_.contains(order.id)) {
    report.rejected = true;
    report.cancelled_quantity = order.quantity;
    return report;
  }

  order.sequence = next_sequence_++;

  const Quantity submitted = order.quantity;
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
