# tickforge

**A low-latency limit order book and matching engine in C++20, with a Python
research layer that uses the engine to study order book imbalance and backtest
market-making strategies.**

[![ci](https://github.com/rakshitmeshram/tickforge/actions/workflows/ci.yml/badge.svg)](https://github.com/rakshitmeshram/tickforge/actions/workflows/ci.yml) ![C++20](https://img.shields.io/badge/C%2B%2B-20-blue) ![tests](https://img.shields.io/badge/tests-differential%20fuzz%20%2B%20ASan%2FUBSan-green) ![deps](https://img.shields.io/badge/runtime%20deps-none-lightgrey)

| | |
|---|---|
| **Throughput** | **27 M msgs/s** single core on a 2017 laptop CPU, 3.7× a `std::map` book on identical flow |
| **Latency** | **p50 42 ns · p99 111 ns · p99.9 171 ns** per message (add / cancel / amend / market) |
| **Correctness** | Unit tests plus differential fuzzing: 1.4 M random messages (4.6 M in CI), including self-trade prevention across several owners, must match an independent reference book trade for trade. Clean under ASan, UBSan and `-Werror` on GCC and Clang, and built and tested with MSVC, in CI on every push |
| **Features** | GTC / IOC / FOK limits, market orders, O(1) cancel, amend, and self-trade prevention (cancel newest / oldest / both) |
| **Research** | Level-1 imbalance has an out-of-sample IC of 0.23 at 50 messages. An inventory-skewed market maker earns money where a symmetric one loses, and using the imbalance signal for quoting *reduces* PnL. The analysis shows why |

All benchmark numbers come from `scripts/reproduce.ps1` on a Dell Inspiron 5577 laptop: Intel Core i7-7700HQ (Kaby Lake, 4 cores / 8 threads, 2.8 GHz base, 3.8 GHz turbo, 6 MB L3), 8 GB RAM, Windows 10 22H2, on AC power with the Balanced power plan. The compiler is Clang 22.1
(`zig c++` 0.17.1), `-O3`. See [Benchmarks](#benchmarks) for the method and run-to-run spread. The
research numbers are deterministic: rerunning with the same seeds reproduced `results/*_metrics.json`
exactly. The order flow behind them is identical on every compiler CI covers (GCC and Clang on
Linux, Clang and MSVC on Windows), and `test_determinism` checks that on every push.

---

## Contents

```
include/tickforge/
  order_book.hpp       fast book: dense price ladder, hierarchical bitset, intrusive FIFO queues
  reference_book.hpp   std::map + std::list book with identical semantics (test oracle and baseline)
  hier_bitset.hpp      64-ary bitset: next/prev occupied price level in O(log64 N)
  id_map.hpp           open-addressing OrderId -> slot map, backward-shift deletion
  flow_generator.hpp   reproducible synthetic order flow with a hidden fair value and informed takers
  message.hpp          32-byte binary message format and file I/O
  c_api.h              exception-free C ABI
src/c_api.cpp          C ABI implementation -> libtickforge_c.so
tools/                 tf_gen (write flow), tf_replay (export book/trade CSVs), tf_bench
tests/                 scenario tests (run on both books), structure property tests, differential fuzz,
                       input validation, cross-platform determinism
scripts/               reproduce.sh / reproduce.ps1, zig/ compiler wrappers for Windows without Visual Studio
.github/workflows/     CI: GCC, Clang, MSVC, sanitizers, Python, research smoke test, clang-format
python/tickforge/      ctypes bindings (numpy only), pytest suite
research/              obi_signal.py, market_maker.py, tune_mm.py (κ/β tuning), plot_latency.py
docs/DESIGN.md         design decisions and trade-offs
```

## Quick start

```bash
git clone https://github.com/rakshitmeshram/tickforge.git && cd tickforge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
(cd build && ctest --output-on-failure)        # C++ tests
(cd python && python3 -m pytest -q)            # Python binding tests
./build/tf_bench --n 2000000                   # benchmark vs std::map baseline
scripts/reproduce.sh                           # every number and figure below
```

Sanitizer build: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTF_SANITIZE=ON`.

**Windows** (Visual Studio 2022 or MinGW-w64, CMake 3.20+), from PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
cd python; python -m pytest -q
.\scripts\reproduce.ps1        # Windows equivalent of reproduce.sh
```

The DLL is written to `python/tickforge/` (`tickforge_c.dll` with MSVC, `libtickforge_c.dll` with
MinGW, which links its C++ runtime statically). With MSVC, warnings are not errors by default; pass
`-DTF_WERROR=ON` to make them so. MSVC sanitizer builds get AddressSanitizer only (no UBSan).

**Windows without Visual Studio.** Everything can come from pip inside a project virtual
environment, with `zig c++` (Clang) as the compiler. The wrappers in `scripts/zig/` call the zig from
`.venv`:

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install ziglang cmake ninja numpy pandas matplotlib pytest
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="$PWD/scripts/zig/zig-cxx.cmd" `
  -DCMAKE_AR="$PWD/scripts/zig/zig-ar.cmd" -DCMAKE_RANLIB="$PWD/scripts/zig/zig-ranlib.cmd"
cmake --build build
ctest --test-dir build --output-on-failure
```

Once `build\` is configured this way, `reproduce.ps1` reuses it. `tf_bench` times messages with
the CPU's TSC, so latency percentiles on Windows are as fine-grained as on Linux, even though
Windows' `steady_clock` only ticks every 100 ns.

The engine needs only a C++20 compiler and CMake. The Python layer needs
`numpy`, and the research scripts also need `pandas` and `matplotlib`.

```python
from tickforge import OrderBook, BUY, SELL, Stp, Tif

book = OrderBook(min_price=1, max_price=19_999)
book.limit(1, SELL, 10_001, 5)
book.limit(2, SELL, 10_002, 5)
r = book.limit(3, BUY, 10_002, 7, Tif.IOC)   # Report(status=OK, filled=7, resting=0)
book.take_trades()                           # numpy structured array: maker_id, taker_id, price, qty, side

# Self-trade prevention: orders with the same non-zero owner never trade with each other.
book.limit(4, SELL, 10_003, 5, owner=7)
r = book.limit(5, BUY, 10_003, 5, owner=7, stp=Stp.CANCEL_OLDEST)  # cancels order 4 instead
book.take_self_trade_cancels()               # order_id, qty of resting orders cancelled by STP
```

## The engine

**Semantics.** Price-time priority. Limit orders support GTC, IOC and FOK (FOK checks liquidity
before touching the book). Market orders never rest. Cancel is O(1). Amend follows common exchange
rules: reducing quantity at the same price keeps queue position, while any other change is a
cancel/replace that loses priority and can trade immediately. Rejections are typed: duplicate id,
invalid price or quantity, unknown order, FOK unfillable, capacity, invalid message.

**Self-trade prevention.** Every order can carry a 32-bit owner id (0 = anonymous). An incoming
order never trades with a resting order of the same non-zero owner. Its STP mode decides what
happens instead:

| mode | resting order | incoming order |
|---|---|---|
| `CancelNewest` (default) | stays | remaining quantity cancelled, never rests |
| `CancelOldest` | cancelled | keeps matching |
| `CancelBoth` | cancelled | remaining quantity cancelled |

Fills against other owners made before the self-match stand. Quantity cancelled from the incoming
order is reported in `ExecReport::self_trade_cancelled`. Each cancelled resting order is delivered
to the trade sink as a `StpCancel` event, if the sink accepts that type (in Python:
`take_self_trade_cancels()`). FOK counts only the liquidity it could actually reach under its STP
mode. An amend that re-enters the book keeps the order's owner and mode. The owner and mode fill
what used to be padding in the 32-byte message format, so older message files still read as
anonymous. The owner is stored inside the 32-byte order node: quantities are capped at 2³⁰, so a
node's quantity fits in 32 bits. Matching for anonymous flow costs one predictable branch, and the
benchmark difference was within run-to-run noise.

**Input validation.** Quantities must be in `[1, 2^30]` (`kMaxQty`), which keeps every level and
liquidity total far from `int64` overflow. Market orders may not reuse the id of a resting order,
so every trade's taker id is unambiguous. A message whose type, side or TIF byte is out of range is
rejected with `RejectedInvalidMessage` before it touches the book, both in `apply()` and at the C
ABI. `read_messages` checks the header's record count against the file size before allocating and
validates every record, so a truncated or corrupt file fails cleanly.

**Data structures** (full reasoning in [docs/DESIGN.md](docs/DESIGN.md)):

| Problem | `std::map` baseline | tickforge |
|---|---|---|
| price → level | red-black tree, O(log L) pointer chasing | dense array indexed by `price - min_price`, O(1) |
| next best level after one empties | `begin()` of tree | 3-layer 64-ary bitset, `countr_zero`/`countl_zero`, O(log64 N) |
| FIFO at a level | `std::list`, allocation per order | intrusive doubly linked list of 32-bit pool indices, no allocation after construction |
| id → order (cancel/amend) | `std::unordered_map`, node per entry | flat open-addressing table, splitmix64, backward-shift deletion |
| trade callback | — | template parameter, inlined (no virtual call) |

**A bug the benchmark caught.** The first version sized the id hash table for `max_orders`: 4M
slots, 64 MB, for a book that holds about 2,000 orders. Almost every lookup then misses the caches
and the TLB. Sizing the table to the expected live order count and letting it grow is
`BookConfig::expected_orders`, and `tf_bench --expected_orders 2097152` reproduces the old sizing.
On the benchmark machine the oversized table more than doubles the fast book's p50 (102 ns vs
42 ns) and cuts throughput from 27 to 12 M msgs/s. The fast book still beats `std::map` at the
median (102 ns vs 139 ns) on this machine.

## Benchmarks

**Machine:** a Dell Inspiron 5577 laptop: Intel Core i7-7700HQ (Kaby Lake, 4 cores / 8 threads, 2.8 GHz base, 3.8 GHz turbo, 6 MB L3), 8 GB RAM, Windows 10 22H2, on AC power with the Balanced power plan. Clang 22.1 (`zig c++` 0.17.1), `-O3`, no `-march=native`. A laptop is
a noisy place to benchmark: there is no core isolation, turbo boost changes the clock, and Windows
runs background work. That is why the spread across runs is given below, not just one run.

**Method.** `tf_bench` replays an in-memory stream of 2M messages through each book. The stream comes
from the flow generator: 60% limit, 31% cancel, 6% market, 3% amend (deep book: 57 / 34 / 6 / 3).
Every cancel and amend targets an order that is resting. Throughput is the best of 5 untimed replays
(3 on the deep book). Latency brackets every message with `lfence; rdtsc; lfence`. The TSC is
invariant on this CPU (checked with CPUID) and is calibrated against `steady_clock`, which gives
0.36 ns resolution. The median cost of an empty bracket (11–13 ns) is subtracted. Without an
invariant TSC, `tf_bench` falls back to `steady_clock` and prints its resolution. On Windows that is
100 ns, too coarse for per-message latency.

**Typical book** (about 2,250 resting orders around the touch):

| | throughput | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| **tickforge::OrderBook** | **27.0 M/s** | **42 ns** | **62 ns** | **111 ns** | **171 ns** |
| std::map baseline | 7.2 M/s | 139 ns | 207 ns | 354 ns | 597 ns |

Fast-book p50 / p99 by type: limit 39 / 109 ns, cancel 44 / 81 ns, market 50 / 135 ns, amend 70 / 145 ns.

**Deep, wide book** (about 123,000 resting orders spread over hundreds of levels; `--live 200000 --decay_pct 2`):

| | throughput | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| **tickforge::OrderBook** | **9.4 M/s** | **107 ns** | **191 ns** | **442 ns** | **600 ns** |
| std::map baseline | 2.6 M/s | 345 ns | 598 ns | 1,142 ns | 3,471 ns |

**Run-to-run spread.** The tables show the run saved in `results/bench.json` and
`results/bench_deep.json`. Across three back-to-back runs, fast-book throughput was 27.0–27.6 M/s
(typical) and 9.4–9.5 M/s (deep). p50 moved by at most 1 ns and p99 by at most 5 ns. The speedup was
3.7–3.8× on the typical book and 3.5–3.6× on the deep one. The baseline's p99.9 is the noisiest
number: 597–785 ns typical, 2.2–3.5 µs deep. One more run, started straight after a rebuild and the
test suite, was slower: 24.3 M/s, p99 183 ns, speedup 3.6×. Expect throughput to vary by 10–15%
between sessions on a laptop.

Both books slow down on the deep book because the working set no longer fits in cache, and the
throughput gap stays about 3.5×.

![latency tail](results/latency_tail.png)

The flat region near 10⁻⁴ (1–15 µs) that both books share is the operating system (interrupts,
scheduler ticks, Windows background work), not the engine. Measuring below it needs core
isolation, which is listed under next steps.

## Testing

* **Scenario tests** (`test_order_book.cpp`): 19 hand-checked scenarios (37 test cases) covering priority, partial
  fills, IOC/FOK, market sweeps, amend priority rules, rejections, capacity, boundary prices, and
  every self-trade prevention mode, including FOK and amend. Every scenario runs against **both**
  books, so the oracle is pinned too.
* **Property tests** (`test_structures.cpp`): `HierBitset` against `std::set` and `IdMap` against
  `std::unordered_map`, over hundreds of thousands of random operations. The bitset is checked at
  word-boundary sizes (63/64/65/4096/4097 bits) and the map starts tiny to force repeated rehashing.
* **Differential fuzz** (`test_differential.cpp`): identical message streams go to `OrderBook` and
  `ReferenceBook`, and every `ExecReport`, every trade in order, top of book after every message and
  full depth every 64 messages must match. There are two stream sources. The adversarial one uses a
  narrow price band with duplicate and stale ids, invalid prices and quantities, and tiny capacity.
  Orders are spread over three owners plus anonymous flow with random STP modes, so self-trade
  prevention fires constantly, and `StpCancel` events must match too.
  The realistic one uses the flow generator. A further test checks that the generator never sends a
  cancel or amend for an order that is not resting, and that its live set equals the book. Set `TF_FUZZ_SEEDS` for longer runs; CI uses 200.
  *Mutation check:* flipping one comparison in best-ask maintenance makes both differential tests
  fail, the adversarial one at message 21 and the realistic one at message 4, so they catch real
  bugs.
* **Sanitizers / warnings:** ASan + UBSan build, `-Wall -Wextra -Wconversion -Wsign-conversion
  -Wold-style-cast … -Werror`, clean on GCC 13 and Clang. CI also builds and tests with MSVC on Windows.
* **Validation tests** (`test_validation.cpp`): malformed enum bytes, the quantity cap, market-order
  id reuse and the capacity invariant on both books. Corrupt message files are covered too:
  truncated, trailing bytes, huge record count, bad magic and bad records.
  The fuzzer also injects malformed bytes, over-cap quantities and reused market ids.
* **Determinism** (`test_determinism.cpp`): hashes every field of five seeded generator streams,
  including the full 2M-message stream behind the imbalance study. They are compared with hashes
  pinned on the benchmark machine, so every CI job checks that the same seed gives the same order
  flow on its compiler and OS. Its first CI run found a real bug: price and quantity were drawn as
  two arguments of one call, whose evaluation order C++ leaves unspecified, so GCC and MSVC builds
  generated different order flow from Clang. See [DESIGN.md §10](docs/DESIGN.md).
* **Python:** pytest covers the FFI boundary, including generator reproducibility, id-range
  isolation, rejection of invalid sides and use of a closed book.
* **CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every push: GCC and Clang,
  each in Release and in an ASan + UBSan build, MSVC Release with the Python tests, a Python job that
  also runs a research smoke test, and a `clang-format` 18 check. Before pushing, format with
  `clang-format -i include/tickforge/*.hpp src/*.cpp tools/*.cpp tests/*.cpp` (version 18, as on
  Ubuntu 24.04; `pip install clang-format==18.1.8` gives the same binary).

## Research 1 — does order book imbalance predict the mid?

`tf_replay` exports a top-5 snapshot after each of 2M messages. `research/obi_signal.py` tests three
signals against the forward mid-price move, using only data a participant could see:

* **OBI1** = (bid₁ − ask₁)/(bid₁ + ask₁) on level-1 quantities
* **OBI5**, the same ratio on top-5 quantities
* **microprice − mid**

Method: chronological 70/30 split. Scaling is fit on train only. IC is measured on test with
**non-overlapping** samples (stride = horizon), so t-stats are not inflated by overlap.

| horizon (msgs) | 1 | 5 | 20 | 50 | 100 | 250 |
|---|---:|---:|---:|---:|---:|---:|
| OBI1 IC | 0.048 | 0.101 | 0.175 | **0.228** | **0.232** | 0.053 |
| microprice IC | 0.035 | 0.075 | 0.133 | 0.185 | 0.190 | 0.022 |
| OBI5 IC | −0.005 | −0.014 | −0.036 | −0.066 | −0.104 | **−0.209** |

![imbalance study](results/obi_signal.png)

Findings:

* Level-1 imbalance is strongly predictive out of sample: t ≈ 30 at 20 messages on 30k independent
  samples. When signal and move are both non-zero, the sign is right 73% of the time.
* Deep imbalance has the **opposite** sign. When the hidden fair value moves, orders placed for the
  old price stay behind on the side the price moved away from. Top-5 depth then piles up on the
  wrong side and works as a contrarian indicator.
* Microprice carries the same information as OBI1 but is slightly weaker, because it is OBI1 scaled
  by a spread that is almost always one tick.

*Caveat:* the flow is synthetic, and the generator's informed takers are what make imbalance
predictive. The point of the exercise is the methodology (no leakage, honest sample counts) and the
tooling, not a claim about real markets.

## Research 2 — market making against the live engine

`research/market_maker.py` places a market maker's quotes **into the same C++ book** that the
synthetic flow trades against, through the ctypes API. Quotes queue behind existing liquidity, fill
only when takers reach them, and get adversely selected by informed flow. Nothing is filled at mid
or by assumption. Every strategy uses 5-lot quotes, a ±50-lot position limit, post-only pricing,
and a cancel/replace only when the price changes, so unchanged quotes keep their queue position.

* **symmetric**: quotes around the mid.
* **inventory**: Avellaneda–Stoikov-style reservation price r = mid − κ·q.
* **inv+OBI**: r = mid − κ·q + β·OBI1, leaning on Research 1's signal.

**Tuning.** `research/tune_mm.py` chooses κ and β on tuning seeds 101–103 only, at the reporting
horizon, by mean final PnL (`results/mm_tuning.json`). For inventory it searches κ from 0.01 to 2.
For inv+OBI it searches a joint κ × β grid with β > 0, so the signal gets its best chance. A first,
narrower grid put both optima on its edge, so the grid was widened before anything was run on the
reporting seeds. The result is κ = 1.0 for inventory, and κ = 0.75, β = 0.025 for inv+OBI.

Two properties of the setup matter for reading these values. Quotes are whole ticks and inventory
moves in 5-lot steps, so κ ≥ 1 saturates: any position moves r by at least 5 ticks, and the maker
then only quotes the side that reduces it, at the touch. κ = 1.0, 1.5 and 2.0 give identical
results. For the same reason, any small β > 0 acts as a switch rather than a dial. It steps one
quote back by a tick whenever imbalance points against that side, and every β from 0.025 to 0.25
gave the same result at κ = 0.75. Larger β is worse at every κ, and at β = 1 PnL is negative for
every κ up to 1.0.

The table reports seeds 1–10, which were never used for tuning: 30 runs and 60.6M engine messages
(141 s on the benchmark laptop).

| | PnL (ticks×lots) | Sharpe / √1k steps | filled lots | mean \|inv\| | markout 100 | markout 1k | markout 10k |
|---|---:|---:|---:|---:|---:|---:|---:|
| symmetric | −14,632 ± 4,537 | −0.63 | 55,759 | 24.8 | +0.07 | −0.08 | −0.25 |
| **inventory** | **+1,996 ± 225** | **+0.67** | 34,244 | 2.4 | +0.10 | +0.08 | +0.06 |
| inv+OBI | +1,125 ± 242 | +0.46 | 18,301 | **1.4** | **+0.18** | **+0.11** | +0.06 |

(± is the standard deviation across 10 seeds. Markout is the mid move in our favour per filled lot,
N messages after the fill.)

![market maker](results/market_maker.png)

Findings:

* **Inventory control is the difference between losing and earning.** The symmetric maker sits at
  its position limit most of the time (mean |inv| 25 of 50) and bleeds as informed flow pushes the
  price through it. Skewing quotes by inventory keeps |inv| near 2 and turns PnL positive in every
  seed (+1,596 to +2,393).
* **A predictive signal improves each fill but lowers total PnL.** Tuned inv+OBI earns less than
  inventory on all 10 seeds (by 871 on average, paired t ≈ 12). Its fills are better: the
  100-message markout is +0.18 against +0.10, and it is never worse at 10k messages (+0.06 for both).
  The cost is volume. Stepping a quote back whenever imbalance points against it avoids the fills
  the signal expects to lose on, but also many that would have paid the spread, and filled lots
  drop by 47%. The edge per fill does not make up for half the fills.
* **Leaning harder on the signal loses money.** Before re-tuning, the parameters were κ = 0.08 and
  β = 0.5, tuned with the v1.0 generator. With them, inv+OBI lost money on the same seeds 1–10
  (−2,266). Its fills looked excellent at 100 messages (+0.41) and toxic at 10k (−0.34), and filled
  volume fell by 73%. The tuning grid shows the same pattern on the tuning seeds: PnL falls steadily
  as β grows.
  The signal's horizon (Research 1: IC peaks at 50–100 messages) is shorter than the time the maker
  holds inventory, so quoting on it mostly buys short-term fill quality at the price of volume.
* The accounting is checked: final PnL equals Σ fills × (final mid − fill price) exactly.

## What this is not

Being clear about scope matters more than overclaiming:

* **Single-threaded, single instrument.** That is deliberate: a matching engine is a sequential state
  machine, and real systems scale by sharding instruments across cores. There is no network gateway
  (FIX/OUCH), no persistence or journaling, and no market data publisher.
* **Synthetic data.** The flow generator is a stylised model chosen to be reproducible. Running the
  research on real data (for example LOBSTER/ITCH files) is the obvious extension. `tf_replay`
  already consumes a binary message format, so this only needs a parser.
* **Benchmarks come from one laptop without core isolation.** The latency numbers subtract a
  measured 11–13 ns timer overhead. Absolute numbers will differ on other machines; the speedup over
  the baseline is the more portable figure.

## Next steps

* An ITCH 5.0 parser so the same engine and research run on real NASDAQ sample data.
* Core pinning and isolation, to measure below the operating system's noise floor (timing
  already uses `rdtsc`).
* A lock-free SPSC ring buffer between a gateway thread and the matching thread.
* Queue-position-aware fill modelling and a fee/rebate model for the market maker.

## License

MIT
