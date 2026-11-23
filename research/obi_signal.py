"""Does order book imbalance predict short-horizon mid-price moves?

Input: the book snapshot CSV written by `tf_replay` (one row per message).
Uses only data an exchange participant could see (top 5 levels), never the
generator's hidden fair value.

Signals
  OBI1   = (bid_qty1 - ask_qty1) / (bid_qty1 + ask_qty1)          in [-1, 1]
  OBI5   = same with the top-5 aggregated quantities
  micro  = microprice - mid, where
           microprice = (bid * ask_qty1 + ask * bid_qty1) / (bid_qty1 + ask_qty1)

Target: mid(t + h) - mid(t) in ticks, for event horizons h.

Method
  * Chronological 70/30 split. Signal scaling (OLS slope) is fit on the first
    70% only; everything reported for "test" is out-of-sample.
  * Information coefficient (Pearson corr of signal vs forward move) is
    computed on NON-overlapping samples (stride h) so the sample count is
    honest; t-stat = IC * sqrt(n).
  * Hit rate counts only samples where both signal and move are non-zero.

Usage: python research/obi_signal.py data/book.csv results/
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _style  # noqa: E402

HORIZONS = [1, 5, 20, 50, 100, 250]


def load(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    df["mid"] = 0.5 * (df.bid + df.ask)
    df["spread"] = df.ask - df.bid
    q1 = df.bid_qty1 + df.ask_qty1
    df["obi1"] = (df.bid_qty1 - df.ask_qty1) / q1
    df["obi5"] = (df.bid_qty5 - df.ask_qty5) / (df.bid_qty5 + df.ask_qty5)
    df["micro"] = (df.bid * df.ask_qty1 + df.ask * df.bid_qty1) / q1 - df.mid
    return df


def evaluate(df: pd.DataFrame, signal: str, h: int, split: int) -> dict:
    fwd = df.mid.shift(-h) - df.mid
    x, y = df[signal].to_numpy(), fwd.to_numpy()
    valid = ~np.isnan(y)
    idx = np.arange(len(df))
    train = valid & (idx < split - h)  # no target leakage across the split
    test = valid & (idx >= split)

    beta = float(np.dot(x[train], y[train]) / np.dot(x[train], x[train]))

    stride = np.zeros(len(df), dtype=bool)
    stride[::h] = True
    xs, ys = x[test & stride], y[test & stride]
    ic = float(np.corrcoef(xs, ys)[0, 1])
    pred = beta * x[test]
    resid = y[test] - pred
    r2 = 1.0 - float(np.sum(resid**2) / np.sum((y[test] - y[test].mean()) ** 2))
    nz = (np.sign(x[test]) != 0) & (np.sign(y[test]) != 0)
    hit = float(np.mean(np.sign(x[test][nz]) == np.sign(y[test][nz])))
    return {"signal": signal, "h": h, "beta": beta, "ic": ic, "t_stat": ic * np.sqrt(len(xs)),
            "n_indep": int(len(xs)), "r2_oos": r2, "hit_rate": hit}


def main(book_csv: str, out_dir: str) -> None:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    df = load(book_csv)
    split = int(0.7 * len(df))

    rows = [evaluate(df, s, h, split) for s in ("obi1", "obi5", "micro") for h in HORIZONS]
    res = pd.DataFrame(rows)
    print(f"{len(df):,} snapshots, median spread {df.spread.median():.0f} ticks; train/test split at {split:,}\n")
    print(res.to_string(index=False, float_format=lambda v: f"{v:.4f}"))
    (out / "obi_metrics.json").write_text(json.dumps(rows, indent=2))

    # --- figure: IC by horizon + conditional mean move by OBI1 decile ------
    import matplotlib.pyplot as plt

    _style.apply()
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10.5, 3.6))
    fig.subplots_adjust(wspace=0.3)
    names = {"obi1": "OBI (level 1)", "obi5": "OBI (levels 1-5)", "micro": "Microprice - mid"}
    for i, s in enumerate(names):
        r = res[res.signal == s]
        a1.plot(r.h, r.ic, color=_style.SERIES[i], marker="o", markersize=4, label=names[s])
    a1.set_xscale("log")
    a1.set_xticks(HORIZONS, [str(h) for h in HORIZONS])
    a1.minorticks_off()
    a1.axhline(0, color=_style.MUTED, linewidth=0.8)
    a1.set_xlabel("horizon (messages)")
    a1.set_ylabel("out-of-sample IC")
    a1.set_title("Top-of-book imbalance predicts; deep imbalance reverts")
    a1.legend(loc="lower left")

    test = df.iloc[split:].copy()
    h = 20
    test["fwd"] = df.mid.shift(-h).iloc[split:] - test.mid
    test = test.dropna(subset=["fwd"])
    test["decile"] = pd.qcut(test.obi1.rank(method="first"), 10, labels=False) + 1
    g = test.groupby("decile").fwd.agg(["mean", "sem"])
    a2.bar(g.index, g["mean"], width=0.7, color=_style.SERIES[0], edgecolor="white", linewidth=1)
    a2.errorbar(g.index, g["mean"], yerr=1.96 * g["sem"], fmt="none", ecolor=_style.INK, elinewidth=1, capsize=2)
    a2.axhline(0, color=_style.MUTED, linewidth=0.8)
    a2.set_xticks(range(1, 11))
    a2.set_xlabel("OBI (level 1) decile, test set")
    a2.set_ylabel(f"mean mid move over next {h} msgs (ticks)")
    a2.set_title("Mid moves toward the heavier side")
    fig.savefig(out / "obi_signal.png")
    print(f"\nwrote {out / 'obi_signal.png'} and {out / 'obi_metrics.json'}")


if __name__ == "__main__":
    main(*(sys.argv[1:3] if len(sys.argv) >= 3 else ("data/book.csv", "results")))
