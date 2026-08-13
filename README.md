# matchbox

A simulated stock exchange: the matching engine itself — order book, order types, and the
matching rules that decide who trades with whom at what price.

Phases 1 and 2 are complete: a single-threaded, single-symbol engine with limit/market/
cancel order types, two interchangeable matching algorithms (price-time priority and
pro-rata), a synthetic order-flow generator, a correctness suite, and a benchmark harness
with measured results. Multi-symbol support and realistic trader models are Phase 3.

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

**Matching.** `MatchingStrategy` is the seam the two algorithms plug into.
`FifoMatcher` repeatedly takes the oldest order at the best crossing level. It re-reads the
best level from the book on every pass, because filling an order can destroy both that
order and the level containing it.

`ProRataMatcher` keeps price priority across levels — best price first, stopping when the
incoming order no longer crosses — and differs only in how one level is shared out. If the
incoming order covers the level, everything there fills. Otherwise each resting order gets
`floor(incoming × resting / level total)`, and the units stranded by flooring go one apiece
to the oldest orders. That rounding remainder is the only place time priority survives.

Two properties make that loop safe. Flooring strands fewer units than there are orders, and
each floored share is strictly below its own order's size whenever the incoming quantity is
below the level total — so a single pass places the whole remainder and no order is pushed
past what it has resting. And each level either clears completely or exhausts the incoming
order, so the sweep always makes progress.

Real pro-rata venues usually add a guaranteed minimum fill for the top time-priority order.
That is deliberately not implemented here; the benchmark below shows exactly the problem it
exists to solve.

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

Pro-rata's randomised runs check the full allocation, not just the invariants: trades
arrive grouped by level, so each run of equal prices is one allocation and is compared
element-for-element against the model's version of that level. That model allocation is
transcribed from the specification rather than obtained by calling the matcher, so it
catches a matcher that reads mutated book state, iterates the wrong way, or drops the
remainder — though it would not catch a misunderstanding of the rule shared by both. The
hand-written cases are what pin down the rule itself.

Six deliberate defects were injected into the allocation logic. Five — remainder handed to
the newest orders instead of the oldest, shares rounded up instead of down, the remainder
pass dropped, the level walked FIFO-style instead of proportionally, and an off-by-one in
the level total — are each caught by both the hand-written cases and the randomised runs.
The sixth, removing the per-order cap in the remainder pass, changes nothing observable:
that guard is unreachable given floor division, by the argument in the Design section. It
is kept because it stops being unreachable the moment the rounding changes.

Assertions are kept live in optimised builds, since they encode the engine's internal
preconditions. Benchmark builds should configure with `-DMATCHBOX_ENABLE_ASSERTS=OFF` so
timings are not distorted.

## Benchmarking

```sh
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DMATCHBOX_ENABLE_ASSERTS=OFF
cmake --build build-bench
./build-bench/bench/matchbox_bench
```

Both strategies replay one identical, pre-recorded stream of 500k actions, so the
comparison is not confounded by different random draws. Every `submit`/`cancel` is timed
individually into a preallocated buffer, after an untimed 50k-action warm-up. The harness
prints its own build type and whether engine assertions were live, so a set of numbers
cannot be quoted without its configuration.

Apple M-series, AppleClang 21, `-O3`, assertions off. Throughput varies about 5% run to
run; `fills/order` and `maxfill` are deterministic for a given stream.

| config | strategy | actions/s | mean ns | p50 ns | p99 ns | p99.9 ns | fills/order | max fill | resting |
|---|---|---|---|---|---|---|---|---|---|
| default | price-time | 7,715,742 | 130 | 83 | 375 | 833 | 0.8 | 8 | 32,007 |
| default | pro-rata | 1,161,149 | 861 | 125 | 7,917 | 13,375 | 34.7 | 439 | 33,121 |
| aggressive | price-time | 8,565,806 | 117 | 83 | 333 | 458 | 0.9 | 10 | 14 |
| aggressive | pro-rata | 6,684,082 | 150 | 83 | 541 | 917 | 1.7 | 48 | 15 |

**What the numbers show.** The expected cost difference is that price-time does O(1) work
per order it actually fills, while pro-rata touches every order resting at a level it only
partly consumes. That does show up, and it is governed by book depth rather than by the
algorithm alone.

Under the default flow the book holds ~32k resting orders, levels are deep, and pro-rata
splits each incoming order across 34.7 makers on average against price-time's 0.8 — about
43× the fill work. Throughput drops 6.6× and p99 latency rises 21× (375 ns to 7,917 ns).
The throughput gap is smaller than the fill-work ratio because the work both strategies
share — the top-of-book lookup, resting the remainder, building the execution report — does
not scale with makers touched.

Under the aggressive flow the same code is nearly as fast as price-time: 6.68M vs 8.57M
actions/s, a 1.28× gap. That flow keeps the book nearly empty (~15 resting orders), so
levels hold one or two orders and there is almost nothing to split across. Pro-rata is not
inherently slow; it is slow in proportion to level occupancy.

The worst single order is the sharpest version of this: one incoming order was split across
**439 makers**, producing 439 trade records where price-time's worst case was 8. That is
the tail that drives p99.9 to 13.4 µs, and it is precisely why real pro-rata venues bolt on
a minimum-fill rule — without one, a deep level converts a single order into hundreds of
economically trivial fills that every downstream system then has to carry.

**Measurement caveats.** `steady_clock` here has 41 ns granularity, and two bracketing
clock reads cost about that much. Three of the four p50 figures sit at 83 ns — two ticks —
so p50 is pinned to the clock floor and should not be read as a real difference; mean and
throughput are the trustworthy central measures, and p99/p99.9 are well clear of the floor.
The replayed stream was recorded against a price-time book, since the generator prices
aggressive orders off the touch and draws cancel targets from resting orders. The two
engines therefore diverge slightly as they run, but the effect is small: cancel hit counts
were 68,273 vs 68,256 under the default flow and identical at 135 under the aggressive one.

## Layout

```
src/orderbook/   price levels, FIFO queues, the book and its order-id index
src/matching/    MatchingStrategy interface, price-time and pro-rata matchers, engine
src/generator/   synthetic order flow
tests/           correctness and randomised stress tests, shared independent book model
bench/           latency/throughput harness
```

## License

MIT
