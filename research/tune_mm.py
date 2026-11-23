"""Tune the market maker's kappa and beta on tuning seeds only.

Protocol (fixed before looking at the reporting seeds):
  * Tuning seeds 101-103, at the same horizon as the reported results
    (200k steps each). Reporting seeds 1-10 are never touched here.
  * Objective: mean final PnL across the tuning seeds.
  * inventory: grid over kappa.
  * inv+OBI:   joint grid over kappa x beta with beta > 0, so the imbalance
    signal gets its best chance (beta = 0 would just be `inventory`).
  * Ties are broken by the smaller parameter values.

Usage: python research/tune_mm.py [steps] [workers] [out_dir]
Writes <out_dir>/mm_tuning.json and prints the chosen values.
"""

from __future__ import annotations

import json
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from market_maker import Strategy, run  # noqa: E402

TUNING_SEEDS = (101, 102, 103)
# A first, narrower grid (kappa <= 0.5, beta >= 0.1) put both optima on its
# edge, so the grid was widened until the optimum was interior.
KAPPAS = (0.01, 0.02, 0.04, 0.06, 0.08, 0.10, 0.15, 0.20, 0.30, 0.50, 0.75, 1.0, 1.5, 2.0)
OBI_KAPPAS = (0.04, 0.08, 0.15, 0.20, 0.30, 0.50, 0.75, 1.0, 1.5)
BETAS = (0.025, 0.05, 0.1, 0.25, 0.5, 1.0)


def one(job: tuple[str, float, float, int, int]) -> dict:
    name, kappa, beta, seed, steps = job
    r = run(Strategy(name, kappa=kappa, beta=beta), seed, steps)
    return {"strategy": name, "kappa": kappa, "beta": beta, "seed": seed,
            "pnl": r["pnl"], "sharpe": r["sharpe_per_sqrt1k"], "lots": r["lots"],
            "mean_abs_inventory": r["mean_abs_inventory"]}


def main(steps: int, workers: int, out_dir: str) -> None:
    jobs = [("inventory", k, 0.0, s, steps) for k in KAPPAS for s in TUNING_SEEDS]
    jobs += [("inv+OBI", k, b, s, steps) for k in OBI_KAPPAS for b in BETAS for s in TUNING_SEEDS]
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=workers) as ex:
        rows = list(ex.map(one, jobs, chunksize=1))
    elapsed = time.time() - t0

    grid: dict[tuple[str, float, float], list[dict]] = {}
    for r in rows:
        grid.setdefault((r["strategy"], r["kappa"], r["beta"]), []).append(r)
    cells = []
    for (name, k, b), rs in grid.items():
        cells.append({"strategy": name, "kappa": k, "beta": b,
                      "mean_pnl": sum(r["pnl"] for r in rs) / len(rs),
                      "min_pnl": min(r["pnl"] for r in rs),
                      "mean_sharpe": sum(r["sharpe"] for r in rs) / len(rs),
                      "mean_lots": sum(r["lots"] for r in rs) / len(rs),
                      "mean_abs_inventory": sum(r["mean_abs_inventory"] for r in rs) / len(rs)})

    best = {}
    for name in ("inventory", "inv+OBI"):
        cs = [c for c in cells if c["strategy"] == name]
        best[name] = max(cs, key=lambda c: (c["mean_pnl"], -c["kappa"], -c["beta"]))

    print(f"{len(jobs)} runs, {steps:,} steps each, seeds {TUNING_SEEDS}, in {elapsed:.0f}s\n")
    print("inventory: mean PnL by kappa")
    for c in sorted((c for c in cells if c["strategy"] == "inventory"), key=lambda c: c["kappa"]):
        print(f"  kappa {c['kappa']:5.2f}  pnl {c['mean_pnl']:9.1f}  min {c['min_pnl']:9.1f}  "
              f"sharpe {c['mean_sharpe']:5.2f}  |inv| {c['mean_abs_inventory']:5.2f}")
    print("\ninv+OBI: mean PnL, rows kappa, columns beta")
    print("  kappa " + "".join(f"{b:>9}" for b in BETAS))
    for k in OBI_KAPPAS:
        row = {c["beta"]: c["mean_pnl"] for c in cells if c["strategy"] == "inv+OBI" and c["kappa"] == k}
        print(f"  {k:5.2f} " + "".join(f"{row[b]:9.0f}" for b in BETAS))
    for name, c in best.items():
        print(f"\nbest {name}: kappa={c['kappa']}, beta={c['beta']}, mean pnl {c['mean_pnl']:.1f}, "
              f"min {c['min_pnl']:.1f}, sharpe {c['mean_sharpe']:.2f}")

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "mm_tuning.json").write_text(json.dumps(
        {"steps": steps, "tuning_seeds": TUNING_SEEDS, "objective": "mean final PnL",
         "best": best, "cells": sorted(cells, key=lambda c: (c["strategy"], c["kappa"], c["beta"])),
         "runs": rows}, indent=1))
    print(f"\nwrote {out / 'mm_tuning.json'}")


if __name__ == "__main__":
    a = sys.argv[1:]
    main(int(a[0]) if a else 200_000, int(a[1]) if len(a) > 1 else 4, a[2] if len(a) > 2 else "results")
