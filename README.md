# matchbox

A simulated stock exchange: the matching engine itself — order book, order types, and the
matching rules that decide who trades with whom at what price.

Phase 1 is complete: a single-threaded, single-symbol engine with price-time priority
matching, limit/market/cancel order types, a synthetic order-flow generator, and a
correctness suite. Pro-rata matching and the benchmark harness are Phase 2.

## Build and test

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Catch2 is fetched automatically by CMake; no other dependencies. Requires a C++20
compiler.

## Design

**Order book.** Each side is a `std::map` of price levels ordered so that `begin()` is the
best price, giving O(1) top-of-book access and O(log levels) insertion. A level holds its
orders in a `std::list` in arrival order, so the front of the list is the highest-priority
order under price-time priority. A separate order-id index maps each live order straight to
its position in that list, which makes cancel and fill O(1) amortised rather than a scan.

Level mutation is private to `OrderBook` (via `friend`) because a level's cached total
quantity and the order-id index have to be updated together — handing a mutable level to a
caller would let the two drift apart.

**Matching.** `MatchingStrategy` is the seam that Phase 2's pro-rata matcher plugs into.
`FifoMatcher` repeatedly takes the oldest order at the best crossing level. It re-reads the
best level from the book on every pass, because filling an order can destroy both that
order and the level containing it.

The book cannot end up crossed because matching stops exactly when the incoming order stops
crossing the far touch, and only then is any remainder allowed to rest.

**Execution reports.** `submit` returns filled / resting / cancelled quantities that always
sum to the submitted quantity. That makes order conservation checkable from outside the
engine, which the stress tests rely on.

**Order flow.** Passive orders are posted away from a slowly drifting reference price — bids
below, asks above — so they rest and build depth, as the large majority of real order flow
does. Liquidity is taken by explicitly aggressive orders, by market orders, and by drift
carrying the reference through already-resting orders. Phase 3 replaces this with informed
and noise traders.

## Testing

Alongside conventional unit tests, the suite runs randomised streams of 100k actions
against an independent model of the book, built only from execution reports. The model
shares no code with `OrderBook` on purpose: checking the engine against its own `front()`
would let a bug in the book's queue ordering satisfy the matcher that reads it.

Each step asserts that the book is never crossed, quantity is conserved, market orders
never rest, sweeps consume levels best-first, and — from the model — that every trade hits
the oldest order resting at the best available price.

The suite is mutation-tested. Six deliberate defects (LIFO queueing, trading at the taker's
price, an off-by-one in the cross test, leaked empty levels, overfilling a maker, and
resting a market-order remainder) are each caught by both the unit tests and the randomised
runs.

Assertions are kept live in optimised builds, since they encode the engine's internal
preconditions. Benchmark builds should configure with `-DMATCHBOX_ENABLE_ASSERTS=OFF` so
timings are not distorted.

## Layout

```
src/orderbook/   price levels, FIFO queues, the book and its order-id index
src/matching/    MatchingStrategy interface, FIFO matcher, engine
src/generator/   synthetic order flow
tests/           correctness and randomised stress tests
bench/           latency/throughput harness (Phase 2)
```

## License

MIT
