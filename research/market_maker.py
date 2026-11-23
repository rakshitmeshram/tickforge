"""Market-making backtest on the live C++ engine.

A market maker (MM) quotes into the same OrderBook that the synthetic flow
trades against, so its orders queue behind existing liquidity, get filled
only when takers actually reach them, and suffer real adverse selection from
the generator's partly informed market orders. Nothing is filled "at mid" or
by assumption.

Loop (one step = `EVENTS_PER_STEP` background messages):
  1. Advance the background flow (one C++ call).
  2. Read the trade tape; book fills where the maker id is ours.
  3. Recompute quotes and cancel/replace only orders whose price changed
     (unchanged orders keep their queue position).

Strategies (identical size, risk limit and post-only rule):
  symmetric   quote one tick either side of the reservation price r = mid.
  inventory   Avellaneda-Stoikov-style skew: r = mid - kappa * q, so a long
              position lowers both quotes (sell more readily, buy less).
  inv+OBI     as `inventory`, plus r += beta * OBI1: lean quotes toward the
              side level-1 order book imbalance predicts (the signal
              measured in obi_signal.py). A quote-pulling variant
              (theta) is also implemented.

Metrics per strategy, mean and stdev across seeds:
  PnL (cash + inventory marked to mid, in ticks x lots), Sharpe of per-step
  PnL (annualisation is meaningless for event time, so it is reported per
  sqrt(1000 steps)), filled lots, requotes (each one forfeits queue
  position), mean |inventory|, and markout per filled lot at 100, 1k and
  10k messages (mid move in our favour after the fill; negative = adverse
  selection).

Usage: python research/market_maker.py [steps] [seeds] [out_dir]
"""

from __future__ import annotations

import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import _style  # noqa: E402
from tickforge import BUY, SELL, FlowGenerator, OrderBook  # noqa: E402

EVENTS_PER_STEP = 10
WARMUP_EVENTS = 20_000
MARKOUT_STEPS = (10, 100, 1000)  # x 10 events = 100, 1k and 10k messages
QUOTE_SIZE = 5
MAX_POS = 50
MM_ID_BASE = 1 << 62


@dataclass(frozen=True)
class Strategy:
    name: str
    kappa: float = 0.0  # ticks of reservation-price skew per lot of inventory
    theta: float | None = None  # OBI threshold for pulling a quote
    beta: float = 0.0  # ticks of reservation-price shift per unit of OBI


# kappa and beta were chosen by research/tune_mm.py on tuning seeds 101-103
# (results/mm_tuning.json); the reported results use seeds 1..N, which were
# never used for tuning. Quotes are whole ticks and inventory moves in 5-lot
# steps, so kappa >= 1 saturates (any position shifts r by >= 5 ticks) and
# any small beta > 0 acts as a switch that steps one quote back a tick when
# imbalance points against it.
STRATEGIES = [
    Strategy("symmetric"),
    Strategy("inventory", kappa=1.0),
    Strategy("inv+OBI", kappa=0.75, beta=0.025),
]


class Quote:
    __slots__ = ("oid", "px", "rem")

    def __init__(self) -> None:
        self.oid, self.px, self.rem = 0, None, 0


def run(strategy: Strategy, seed: int, steps: int) -> dict:
    book = OrderBook()
    gen = FlowGenerator(seed)
    gen.step(book, WARMUP_EVENTS)
    book.take_trades()

    next_id = MM_ID_BASE
    requotes = 0
    bid, ask = Quote(), Quote()
    q = 0  # inventory, lots
    cash = 0.0  # ticks x lots
    mids = np.empty(steps)
    pnl = np.empty(steps)
    inv = np.empty(steps)
    fills: list[tuple[int, int, int, int]] = []  # (step, +1 buy / -1 sell, price, qty)

    def replace(quote: Quote, side: int, px: int | None) -> None:
        nonlocal next_id, requotes
        if quote.rem > 0 and px == quote.px:
            return  # keep queue priority
        if quote.rem > 0 and px is not None:
            requotes += 1  # repricing a live quote forfeits its queue position
        if quote.rem > 0:
            book.cancel(quote.oid)
            quote.rem = 0
        quote.px = None
        if px is None:
            return
        next_id += 1
        r = book.limit(next_id, side, px, QUOTE_SIZE)
        if r.ok and r.resting > 0:
            quote.oid, quote.px, quote.rem = next_id, px, r.resting

    for t in range(steps):
        gen.step(book, EVENTS_PER_STEP)

        tr = book.take_trades()
        if tr.size:
            for qt, sign in ((bid, +1), (ask, -1)):
                if qt.rem == 0:
                    continue
                mine = tr[tr["maker_id"] == qt.oid]
                if mine.size:
                    n = int(mine["qty"].sum())
                    q += sign * n
                    cash -= sign * float(np.dot(mine["price"], mine["qty"]))
                    qt.rem -= n
                    fills.append((t, sign, int(mine["price"][0]), n))

        bb, ba = book.best_bid, book.best_ask
        if bb is None or ba is None:
            mids[t] = mids[t - 1] if t else 0.0
            pnl[t] = cash + q * mids[t]
            inv[t] = q
            continue
        mid = 0.5 * (bb + ba)

        obi = 0.0
        if strategy.theta is not None or strategy.beta:
            (b1,) = book.depth(BUY, 1)
            (a1,) = book.depth(SELL, 1)
            obi = (b1[1] - a1[1]) / (b1[1] + a1[1])

        r = mid - strategy.kappa * q + strategy.beta * obi
        bid_px = math.floor(r - 0.5)
        ask_px = math.ceil(r + 0.5)
        bid_px = min(bid_px, ba - 1)  # post-only: never cross
        ask_px = max(ask_px, bb + 1)

        want_bid = q + QUOTE_SIZE <= MAX_POS
        want_ask = q - QUOTE_SIZE >= -MAX_POS
        if strategy.theta is not None:
            if obi < -strategy.theta:
                want_bid = False  # sellers about to take out the bid
            elif obi > strategy.theta:
                want_ask = False

        replace(bid, BUY, bid_px if want_bid else None)
        replace(ask, SELL, ask_px if want_ask else None)

        mids[t] = mid
        pnl[t] = cash + q * mid
        inv[t] = q

    book.close()
    gen.close()

    d = np.diff(pnl, prepend=0.0)
    sharpe = float(d.mean() / d.std() * math.sqrt(1000)) if d.std() > 0 else 0.0
    markouts = {}
    for h in MARKOUT_STEPS:
        mk, lots = 0.0, 0
        for (t, sign, px, n) in fills:
            if t + h < steps:
                mk += sign * (mids[t + h] - px) * n
                lots += n
        markouts[h] = mk / lots if lots else 0.0
    return {
        "strategy": strategy.name,
        "seed": seed,
        "pnl": float(pnl[-1]),
        "sharpe_per_sqrt1k": sharpe,
        "fills": len(fills),
        "lots": int(sum(f[3] for f in fills)),
        "mean_abs_inventory": float(np.abs(inv).mean()),
        "markout_100": markouts[10],
        "markout_1000": markouts[100],
        "markout_10000": markouts[1000],
        "requotes": requotes,
        "pnl_path": pnl[:: max(1, steps // 500)].tolist(),
        "inv_path": inv[:: max(1, steps // 500)].tolist(),
    }


def main(steps: int, seeds: int, out_dir: str) -> None:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    results = []
    t0 = time.time()
    for s in STRATEGIES:
        for seed in range(1, seeds + 1):
            results.append(run(s, seed, steps))
    elapsed = time.time() - t0
    total_events = len(results) * (steps * EVENTS_PER_STEP + WARMUP_EVENTS)
    print(f"{len(results)} runs, {steps:,} steps each ({total_events / 1e6:.1f}M engine messages) "
          f"in {elapsed:.1f}s\n")

    keys = ["pnl", "sharpe_per_sqrt1k", "lots", "requotes", "mean_abs_inventory", "markout_100", "markout_1000", "markout_10000"]
    summary = []
    print(f"{'strategy':<11}" + "".join(f"{k:>21}" for k in keys))
    for s in STRATEGIES:
        rs = [r for r in results if r["strategy"] == s.name]
        row = {"strategy": s.name}
        line = f"{s.name:<11}"
        for k in keys:
            v = np.array([r[k] for r in rs], dtype=float)
            row[k] = {"mean": float(v.mean()), "std": float(v.std(ddof=1)) if len(v) > 1 else 0.0}
            line += f"{v.mean():>11.2f} ± {row[k]['std']:<7.2f}"
        summary.append(row)
        print(line)

    (out / "mm_metrics.json").write_text(json.dumps(
        {"steps": steps, "seeds": seeds, "events_per_step": EVENTS_PER_STEP, "quote_size": QUOTE_SIZE,
         "max_pos": MAX_POS, "summary": summary,
         "runs": [{k: v for k, v in r.items() if not k.endswith("_path")} for r in results]}, indent=2))

    import matplotlib.pyplot as plt

    _style.apply()
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.6))
    fig.subplots_adjust(wspace=0.28)
    hz = [h * EVENTS_PER_STEP for h in MARKOUT_STEPS]
    for i, s in enumerate(STRATEGIES):
        r = next(r for r in results if r["strategy"] == s.name and r["seed"] == 1)
        x = np.linspace(0, steps * EVENTS_PER_STEP / 1e6, len(r["pnl_path"]))
        a1.plot(x, r["pnl_path"], color=_style.SERIES[i], label=s.name)
        row = next(x for x in summary if x["strategy"] == s.name)
        m = [row[f"markout_{h}"]["mean"] for h in hz]
        e = [row[f"markout_{h}"]["std"] for h in hz]
        a2.errorbar(hz, m, yerr=e, color=_style.SERIES[i], marker="o", markersize=5, capsize=3)
    a1.axhline(0, color=_style.MUTED, linewidth=0.8)
    a1.set_title("PnL marked to mid (seed 1)")
    a1.set_xlabel("engine messages (millions)")
    a1.set_ylabel("ticks × lots")
    a2.axhline(0, color=_style.MUTED, linewidth=0.8)
    a2.set_xscale("log")
    a2.set_xticks(hz, [f"{h:,}" for h in hz])
    a2.minorticks_off()
    a2.set_title(f"Markout per filled lot ({seeds} seeds, ±1 sd)")
    a2.set_xlabel("messages after fill")
    a2.set_ylabel("ticks in our favour")
    fig.legend(*a1.get_legend_handles_labels(), loc="upper center", ncol=len(STRATEGIES),
               bbox_to_anchor=(0.5, 1.06))
    fig.savefig(out / "market_maker.png")
    print(f"\nwrote {out / 'market_maker.png'} and {out / 'mm_metrics.json'}")


if __name__ == "__main__":
    a = sys.argv[1:]
    main(int(a[0]) if a else 100_000, int(a[1]) if len(a) > 1 else 5, a[2] if len(a) > 2 else "results")
