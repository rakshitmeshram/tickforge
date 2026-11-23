# Interview notes

## Resume entry

**tickforge — Low-Latency Order Book & Market-Making Research** · C++20, Python, CMake · github.com/rakshitmeshram/tickforge

- Built a price-time priority matching engine in C++20 (limit/market, GTC/IOC/FOK, cancel, amend)
  that processes **27M messages/s** at **42 ns p50 / 111 ns p99** latency on a 2017 laptop CPU,
  **3.7× faster** than a `std::map` baseline on identical order flow.
- Designed allocation-free data structures: a dense price ladder, a 64-ary hierarchical bitset for
  O(log64 N) best-level lookup, an intrusive pooled FIFO, and an open-addressing id map; found and
  fixed a cache-sizing issue that had made p50 latency **2.4× worse**.
- Verified correctness by **differential fuzzing** against an independent reference book (millions
  of random messages compared trade by trade), with ASan/UBSan and `-Werror` on GCC/Clang and an
  MSVC build in CI.
- Exposed the engine to Python via a C ABI and used it to show that level-1 order book imbalance
  predicts mid-price moves (**out-of-sample IC 0.23**, non-overlapping samples, no leakage).
- Backtested market-making strategies against the live engine with real queue priority: inventory
  skew turned **−14.6k into +2.0k** ticks×lots PnL across 10 unseen seeds. Markouts and a tuning
  sweep showed why quoting on the imbalance signal lowered PnL despite better fills.

A shorter version for a one-page resume uses bullets 1, 3 and 5.

## Questions to expect, and the short answers

**Why is it faster than `std::map`?** Price lookup is array indexing instead of a tree walk, there is
no allocation per order or per level, queues are linked by 32-bit indices in one contiguous pool,
and the id lookup is a flat hash table. Most of the win is fewer cache misses, not fewer instructions.

**Why only 3.7× faster?** The benchmark book is small (about 2,250 orders near the touch), so the
tree is shallow and mostly cached, which makes the baseline strong. On a deep book (about 123k
orders) both slow down from cache misses and the gap stays around 3.5×. Quoting the honest number is
better than cherry-picking a pathological baseline.

**How do you find the next price level when the best empties?** With a hierarchical bitset. Each
summary bit marks a non-zero word in the layer below, so finding the next set bit is at most one
`tzcnt` per layer up and down: three layers for 262k levels.

**What was the cache bug?** I sized the id hash table for the maximum order count (4M slots, 64 MB).
Every lookup hit a random slot, causing a cache and TLB miss. Sizing it to the typical live count and
growing on demand took median latency from 102 ns to 42 ns and throughput from 12 to 27 M msgs/s.
`tf_bench --expected_orders 2097152` reproduces the old sizing. The lesson: measure, and size for the
working set.

**Why backward-shift deletion?** Tombstones accumulate under heavy cancel churn (about 31% of
messages here), and lookups have to probe past them. Backward shift keeps probe chains as short as
if the deleted key had never been inserted.

**How do you know it is correct?** Two independent implementations get the same random streams and
must agree on every report, every trade in order, the top of book after every message, and full depth
periodically. I also mutated the code on purpose (one flipped comparison) to confirm the fuzz test
catches it: both differential tests fail, the adversarial one at message 21.

**How did you measure latency?** Fenced `rdtsc` around each message, calibrated against
`steady_clock` (0.36 ns resolution), with the median empty-bracket cost (about 12 ns) subtracted, and
percentiles per message type. `steady_clock` alone was useless on Windows: it ticks every 100 ns. The
flat tail near 10⁻⁴ is OS noise on a laptop. With more time I would add core isolation.

**Why single-threaded?** A book is a sequential state machine. Locks would only add latency. Real
engines shard instruments across pinned cores and feed them through lock-free SPSC queues.

**Your imbalance signal has IC 0.23. Why doesn't quoting on it help?** Tuned, it improves each
fill: the 100-message markout goes from +0.10 to +0.18 ticks, and it is no worse at 10k. But it gets
there by stepping quotes back when imbalance is against them, which halves filled volume (−47%), so
PnL is lower on all 10 seeds (+1.1k vs +2.0k). Leaning harder makes it worse. With the β tuned on
the old generator (0.5), fills were +0.41 at 100 messages but −0.34 at 10k, and the strategy lost
money. The signal's horizon is shorter than the inventory holding period. Signal IC does not equal
strategy PnL, and execution details decide the outcome.

**What was the hardest bug?** One that every unit test missed: the same seed produced different
order flow on GCC and MSVC than on Clang. Price and quantity were both drawn from the RNG as two
arguments of one call, and C++ leaves argument evaluation order unspecified. A CI test that hashes
seeded streams on every compiler and compares them with pinned values caught it on its first run.
Clang's order was kept, so no published result changed.

**How did you avoid overfitting the strategy?** κ and β are tuned by a committed script
(`research/tune_mm.py`) on seeds 101–103, and results are reported on seeds 1–10, which were never
used for tuning. When the first grid put the optimum on its edge, I widened it before touching the
reporting seeds. The research split is chronological 70/30,
with IC computed on non-overlapping samples.

**Isn't synthetic data a weakness?** Yes, and the README says so. The generator is useful because
it is controlled: I know the ground truth (informed takers exist) and can check that the tooling
detects it. The next step is an ITCH parser to run the same pipeline on real data.
