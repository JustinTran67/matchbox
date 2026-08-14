# matchbox

A simulated stock exchange: the matching engine itself — order book, order types, and the
matching rules that decide who trades with whom at what price.

Phases 1-4 are complete: an engine with limit/market/cancel order types, two
interchangeable matching algorithms (price-time priority and pro-rata), informed and noise
trader populations, a correctness suite, a benchmark harness, a concurrent multi-symbol
simulation, and the whole thing containerised behind Kafka on Kubernetes - all with
measured results from real runs. AWS and observability are Phase 5.

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

**Order flow.** Noise traders post passively away from a slowly drifting reference price —
bids below, asks above — so they rest and build depth, as the large majority of real order
flow does. Liquidity is taken by explicitly aggressive orders, by market orders, and by
drift carrying the reference through already-resting orders.

**Informed traders.** An informed trader sees a fundamental the market cannot: a random
walk, separate from the noise traders' own reference price, which is their uninformed
guess rather than the truth. When the book's mid drifts off the fundamental, the informed
trader posts on the correcting side, anchored on the touch it would have to cross and
reaching `correction_aggression` of the way from there to the fundamental. So it crosses
only when the fundamental is already through that touch, and even then stops short rather
than sweeping the book.

Two details matter more than they look. The fundamental advances exactly once per
simulation step whether or not the informed trader acts that step — otherwise a run with
fewer informed traders would also get a slower-moving fundamental, and comparisons across
informed ratios would be meaningless. And the informed trader draws the fundamental's walk
from a *different* RNG stream than its order sizes and sides: sharing one stream would make
the fundamental's path depend on how often the trader was asked to quote, reintroducing
the same bias through the back door. A test asserts both populations reach an identical
fundamental after the same number of steps.

Both populations on a symbol draw order ids from one shared `OrderIdSource`, because an id
is the engine's identity for an order and two populations counting independently would
hand the book duplicates.

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

Concurrency is checked by determinism rather than by absence of crashes. One symbol is run
alone, and its full trade sequence plus the exact book it leaves behind are recorded. The
same symbol is then replayed on four threads at once alongside four unrelated symbols, and
every replica must match the solo run exactly. A shared counter, a shared RNG, or any other
hidden global would perturb that symbol depending on what ran beside it, and this is what
would catch it. The concurrent tests are also run clean under ThreadSanitizer:

```sh
cmake -B build-tsan -DMATCHBOX_TSAN=ON
cmake --build build-tsan
./build-tsan/tests/matchbox_tests "a symbol's outcome*" "an informed trader*" \
    "the fundamental advances*" "order ids stay*"
```

That run initially reported 33 races — all of them inside Catch2's assertion machinery,
because the trace helper called `REQUIRE` from worker threads and Catch2's assertion
handling is not thread-safe. The helper now records a consistency flag and the main thread
asserts on it after `join()`. No race was ever found in the engine or simulator themselves.

Four defects were injected into the informed trader. Inverting the correction direction and
ignoring `correction_aggression` are both caught by the statistical test and the unit
tests. Anchoring on the near touch instead of the far touch and freezing the fundamental are
caught only by unit tests — and the first of those is worth being precise about: measured,
it tracks the fundamental *better* (|px−true| of 5.4 vs 10.6), so it is not a correctness
bug at all but a gentler correction that still satisfies the property under test. Writing a
statistical test to reject it would be testing a design preference, not correctness, so the
exact-price unit test pins the intended anchoring instead.

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

## Multi-symbol simulation

```sh
cmake -B build
cmake --build build
./build/sim/matchbox_sim_driver
```

Twelve symbols run concurrently, one thread each, 200k steps per symbol, at three informed
ratios. Seeds are fixed, so the whole run is bit-reproducible.

| informed | spread | sd | resting orders | \|px−true\| | sd | passive posts/1k | crossing/1k |
|---|---|---|---|---|---|---|---|
| 0.00 | 11.46 | 2.94 | 8,566 | 120.12 | 46.35 | 533.4 | 342.8 |
| 0.20 | 12.02 | 3.39 | 6,169 | 86.94 | 56.78 | 499.4 | 420.3 |
| 0.50 | 21.72 | 12.94 | 5,948 | 26.24 | 17.05 | 581.9 | 507.8 |

`sd` is the spread of per-symbol means across the twelve symbols, not within a run.

**Price discovery works, and it is the unambiguous result.** Mean |trade price − fundamental|
falls from 120.1 to 26.2, a 4.6× improvement, while the dispersion across symbols falls too
(46.4 to 17.1). With no informed traders the book follows the noise traders' own reference
walk, which is independent of the fundamental, so the two drift apart roughly as √steps —
the absolute number at 0.00 is therefore a function of run length and only the comparison
across ratios is meaningful.

**Depth falls and spread widens — and the obvious explanation is wrong.** The natural
suspicion is a confound: raising the informed ratio replaces noise actions with informed
ones, so perhaps the book thins simply because less passive liquidity is being posted. The
measurement says no. Passive posts per 1,000 steps are 533 → 499 → **582**, flat to slightly
*rising*, because informed orders that fail to cross rest near the touch. Meanwhile crossing
submissions climb monotonically, 343 → 508 per 1,000. So liquidity supply holds up while
consumption rises: resting depth falls 8,566 → 5,948 because informed flow is picking off
resting orders, which is adverse selection rather than a supply artefact.

**The spread result is directionally consistent but statistically weak, and should not be
quoted as a headline.** The mean rises 11.46 → 21.72, but the per-symbol standard deviation
rises far faster, 2.94 → 12.94. With twelve symbols the standard error at the top ratio is
about 3.7, so the gap is roughly 2.7 standard errors — suggestive, not settled. The honest
finding is that informed flow makes the touch both wider *and* dramatically more variable
across symbols; the increase in dispersion is the more robust of the two effects.

**Why thread-per-symbol.** Each thread owns one `MarketSimulator`, one `Engine`, and one
exclusive results slot. Nothing is shared: no queue, no lock, no global RNG, no shared id
counter. There is no synchronisation in the hot path because there is nothing to
synchronise — the only happens-before edge in the whole run is `join()`.

That is a deliberate rehearsal for Phase 4 rather than scaffolding to throw away. A symbol
already has no reachable path to another symbol's state, which is exactly the precondition
for running each one as its own process or container; the step from thread to container
changes deployment, not the code's ownership model. It is also why `Order` still carries no
symbol field and there is no routing layer: partitioning order flow by symbol is Kafka's job
in Phase 4, and building a dispatcher now would be replaced by it. The determinism test is
what proves the isolation is real rather than assumed.

## Containerization & orchestration

```sh
# fast loop: compose
docker compose -f service/docker-compose.yml up --build -d
docker compose -f service/docker-compose.yml run --rm e2e

# the real thing: kind
./infra/k8s/deploy.sh
```

Order flow arrives on a 4-partition `matchbox.orders` topic keyed by symbol, and trades go
out on `matchbox.trades`. Keying by symbol is what buys per-symbol ordering: price-time
priority is meaningless if a symbol's own orders can be reordered in transit.

### Why partitions are assigned statically

The engine service calls `assign()` on a fixed partition list rather than `subscribe()`
with a consumer group. Each pod derives its partitions from the ordinal on the end of its
own name (`matchbox-engine-2` -> 2), injected via the downward API, which is why the
workload is a `StatefulSet` rather than a `Deployment` - a Deployment's random pod suffixes
carry no stable identity to derive ownership from.

This is a matching-engine constraint, not a generic microservices preference. Consumer
groups rebalance, and a rebalance has a window where ownership of a partition can overlap
between members. For a stateless consumer that window is harmless: the work is idempotent
and the worst case is duplicated effort. An engine's order book is the opposite - mutable,
in-memory, and the sole authority for its symbol. Two pods believing they own one symbol
would each match against their own divergent copy of that book and publish conflicting
trades for it, with nothing in the pipeline able to detect that the two disagree. Static
assignment removes the window rather than trying to survive it.

The ownership function is small enough to check exhaustively instead of trusting: for every
partition count up to 24 and every replica count up to it, the test asserts each partition
is claimed by *exactly* one ordinal - not "at least one" (a gap silently drops a symbol's
flow) and not "at most one" (an overlap is the split-brain above). Four injected defects -
dropping the remainder offset, never handing out remainder partitions, making ordinals 0
and 1 both claim partition 0, and an off-by-one that double-claims - are each caught by it.

### Books are per symbol, not per partition

A partition can carry more than one symbol, because symbols map onto partitions by hashing
the key. With four symbols and four partitions that happens to come out 1:1 here (verified:
5,000 orders on each of the four partitions), but nothing guarantees it - `AAPL` and `GOOG`
both hash to partition 0, which would have left partition 1 idle and one pod holding two
books. The service therefore keeps an `Engine` per symbol, created on first sight, rather
than one per partition. Correctness never depends on the hash landing evenly; only the
tidiness of the demo does.

### End-to-end verification

Trusting the system's own output would prove nothing, so the check compares it against the
already-proven single-process engine. A fixed-seed stream is recorded per symbol against a
local `Engine`, capturing both the orders and the trades that engine produced. The same
orders are then published to Kafka, and the trades the cluster publishes are diffed against
the local ones, per symbol, in order.

Run against a live `kind` cluster - one Kafka broker and four engine pods, all in-cluster:

```
published 20000 orders, expecting 11606 trades
received 11606 trades (0 malformed)

symbol     expected   observed   result
AAPL           3034       3034   MATCH
MSFT           2886       2886   MATCH
AMZN           3120       3120   MATCH
META           2566       2566   MATCH

PASS: cluster trades match in-process ground truth
```

Exit code 0, four pods ready, zero restarts. Trades landed 3,034 / 2,566 / 3,120 / 2,886
across the four `matchbox.trades` partitions - 11,606, matching the ground truth exactly.

### Limitations

These are real and deliberate, not oversights:

- **No elastic scaling.** Replica count must equal partition count. Changing it without
  reassigning partitions leaves partitions unowned. This is the price of the static
  ownership guarantee above, and it is the right trade for state that cannot be split.
- **Book state is lost on pod restart.** The book lives in memory only. A restarted pod
  resumes from its committed offset with an empty book, so previously resting orders are
  gone. A real venue would rebuild by replaying the partition from the start or restoring a
  snapshot plus a tail of the log; neither is implemented here.
- **At-least-once trade publishing.** Offsets are committed only after outstanding trades
  are flushed and acknowledged, so a crash re-processes rather than drops. The cost is that
  a replay can publish a trade twice - there is no idempotent producer or transactional
  write, and no deduplication downstream.
- **A single Kafka broker.** Replication factor 1, no fault tolerance. Losing the broker
  loses the log.
- **The broker's advertised listener is hard-coded** to the single pod's DNS name. A
  multi-broker StatefulSet would have to derive it per pod.


## Layout

```
src/orderbook/   price levels, FIFO queues, the book and its order-id index
src/matching/    MatchingStrategy interface, price-time and pro-rata matchers, engine
src/generator/   noise and informed trader flow, market simulator, shared id source
tests/           correctness and randomised stress tests, shared independent book model
bench/           latency/throughput harness
sim/             concurrent multi-symbol simulation and market-quality stats
service/         Kafka-facing engine service, wire format, Dockerfile, compose stack
infra/k8s/       Kafka + engine manifests for a local kind cluster
```

## License

MIT
