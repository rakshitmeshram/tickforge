# Design notes

This file explains why the engine looks the way it does, and which alternatives were rejected.

## 1. Integer prices

Prices are `int64` ticks. Floating-point prices make two "equal" prices compare unequal after
arithmetic, which breaks level lookup and priority. Every production venue protocol (ITCH, OUCH,
FIX with fixed-point) carries prices as integers for this reason. Converting to a decimal price
is a presentation concern.

## 2. Dense price ladder instead of a tree

Each side is a `std::vector<Level>` with one slot per tick in `[min_price, max_price]`, so finding
a level is `price - min_price`.

* **Why not `std::map`?** Each lookup walks O(log L) heap-allocated nodes, and each step is a likely
  cache miss. Every new price level also allocates.
* **Cost:** memory is proportional to the price range, not to the number of occupied levels. At 24
  bytes per level, a 20,000-tick band costs about 940 KB for both sides. Real engines handle this with a
  sliding window re-centred on the market, or with per-instrument tick bands. `BookConfig`
  rejects ranges above 2²⁸ ticks.

## 3. Finding the next best level: hierarchical bitset

When the best level empties, the next one has to be found. A linear scan over empty ticks is
O(distance), which is fine for liquid books and bad for gappy ones. `HierBitset` keeps one bit per
level, plus a summary layer where bit *w* means "word *w* below is non-zero", repeated until a
single word remains. Three layers cover 262,144 levels. `next_set`/`prev_set` touch at most one
word per layer on the way up and one per layer on the way down, using
`std::countr_zero`/`countl_zero`, which compile to single `tzcnt`/`lzcnt` instructions.

`set` stops climbing as soon as it finds a word that was already non-zero, and `reset` stops as soon
as a word is still non-zero, so the common case touches one word.

The best price per side is cached. The bitset is consulted only when a best level empties or when
walking depth.

## 4. Orders: pool + intrusive FIFO with 32-bit links

All order nodes are pre-allocated in one `std::vector<Node>` at construction and threaded onto a
free list. A level's queue is a doubly linked list of **indices** into that pool:

* No allocation on the hot path, so no allocator latency spikes and no fragmentation.
* 32-bit indices instead of 64-bit pointers make nodes smaller (32 bytes, two per cache line) and keep the structure
  relocatable.
* The per-order quantity is 32-bit as well; quantities are capped at 2³⁰ and level totals stay
  64-bit. That frees room for the self-trade-prevention owner id and mode inside the same 32 bytes.
  A first version kept them in a separate parallel array. Every resting order then touched a second
  cache line, which cost up to ~15% on the deep-book benchmark. Folding them into the node brought
  the cost down to within noise.
* The free list is LIFO, so the node released by a fill or cancel is the next one reused, and it
  is still in cache.
* Doubly linked, so cancelling from the middle of a queue is O(1).

## 5. OrderId → node: open addressing

`IdMap` is a flat array of `{key, value}` slots with linear probing and a splitmix64 hash (client ids
are often sequential, which would cluster badly under identity hashing).

* **Deletion uses backward shift, not tombstones.** After removing a key, the entries after it in
  the probe chain move back if that does not put them before their home slot. Lookups never step
  over tombstones, and performance does not decay under heavy cancel churn. About 31% of messages
  in the benchmark flow are cancels, all of them for live orders.
* **Sizing is to expected live orders, not maximum orders.** This is the main performance lesson of
  the project. With 4M slots (64 MB), almost every lookup misses the caches and the TLB. On the
  benchmark laptop (i7-7700HQ) that more than doubles p50, from 42 ns to 102 ns, and cuts throughput
  from 27 to 12 M msgs/s. `tf_bench --expected_orders 2097152` reproduces it.

## 6. Trades through a template parameter

`limit(..., Sink&& on_trade)` takes the trade consumer as a template parameter, so the call is
inlined at each fill. A virtual `Listener::on_trade` would cost an indirect call per fill and block
inlining. The C ABI wraps this with a sink that appends to a buffer.

## 7. Amend semantics

Reducing quantity at the same price keeps time priority, because it cannot hurt anyone behind you.
Increasing quantity or changing price is a cancel/replace that goes to the back of the queue and may
trade immediately. This matches mainstream exchange rules, and Research 2 shows it matters:
strategies that reprice often lose queue position and fill less.

## 8. FOK before matching

Fill-or-kill sums opposite liquidity through the limit price **before** touching the book, stopping
as soon as the quantity is covered. The alternative, matching and then rolling back, would emit and
retract trades, which is wrong for any downstream consumer.

## 8a. Self-trade prevention

STP is decided by the incoming order: its owner id and mode, with owner 0 meaning anonymous so
existing flow is unaffected. The check sits inside the per-maker matching loop, behind
`owner != 0`, so anonymous matching pays one predictable branch. Three modes match what major
venues offer: cancel newest, cancel oldest, cancel both. Two choices needed care:

* **Reporting.** Cancelling a resting order is an event its owner must hear about. Trades already
  flow through the sink template parameter, so STP cancels use the same channel as a second event
  type, `StpCancel`. `notify_stp` uses `if constexpr (std::is_invocable_v<...>)`, so existing sinks
  that only take `Trade` compile unchanged and pay nothing.
* **FOK.** The pre-check must count exactly the liquidity matching would reach. It walks individual
  orders when the incoming order has an owner, skipping own orders under cancel-oldest and stopping
  at the first own order otherwise. A FOK therefore still either fills completely or leaves the
  book untouched.

## 9. Correctness strategy: a second, obviously-correct implementation

The optimized book is checked against `ReferenceBook`, a short `std::map`/`std::list` implementation
whose correctness is easy to see by reading it. The differential test compares every observable
output, not only the final state, so a bug that corrupts and later repairs state still shows up. The
adversarial stream deliberately concentrates on edge cases: a narrow price band (lots of crossing),
stale and duplicate ids, invalid inputs, and capacity exhaustion.

## 10. Reproducibility

The flow generator uses its own xoshiro256** generator and its own Box–Muller, geometric and
lognormal transforms, because `std::normal_distribution` and friends are implementation-defined and
give different streams on libstdc++, libc++ and MSVC. On a given platform one seed always gives
the same message stream. Across platforms the streams should match, but the transforms call
`std::log`, `std::cos` and `std::sqrt`, and only `sqrt` is required to be correctly rounded, so a
last-bit difference in a platform's math library could change a price somewhere. This has only been
checked on one machine; comparing a stream hash between the Linux CI job and Windows would settle it.

The generator is a participant that needs feedback: it must know when its orders fill or fail to
rest. Otherwise it keeps "cancelling" orders that no longer exist, and its depth control, which
depends on its live-order count, steers by a count that includes dead orders. v1.0 had exactly this
bug. `FlowGenerator::step()` applies each message and forwards the fills, STP cancels and the
report back to the generator, so its live set always equals its resting orders. A test asserts this
and checks that no cancel or amend is ever rejected as unknown.

## 11. Threading

There is none, on purpose. A single instrument's book is inherently sequential: message *n+1* depends
on the state left by message *n*. Production engines get parallelism by sharding instruments across
pinned cores, each with a single writer, connected to gateways by lock-free SPSC queues. That
belongs to the next layer up (see README, Next steps).
