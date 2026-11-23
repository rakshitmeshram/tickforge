"""Latency distribution plot from `tf_bench --samples` output.

Usage: python research/plot_latency.py results/latency_samples.csv results/bench.json results/
"""

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _style  # noqa: E402


def main(samples_csv: str, bench_json: str, out_dir: str) -> None:
    import matplotlib.pyplot as plt

    df = pd.read_csv(samples_csv)
    meta = json.loads(Path(bench_json).read_text())
    _style.apply()
    fig, ax = plt.subplots(figsize=(6.4, 3.6))
    for i, (name, g) in enumerate(df.groupby("book", sort=False)):
        v = np.sort(g.ns.to_numpy())
        tail = 1.0 - np.arange(1, len(v) + 1) / len(v)
        keep = tail > 0
        ax.plot(v[keep], tail[keep], color=_style.SERIES[i], label=name)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("per-message latency (ns, clock overhead subtracted)")
    ax.set_ylabel("fraction of messages slower")
    ax.set_title(f"Latency tail, {meta['messages']:,} messages")
    for q, lbl in ((0.5, "p50"), (0.01, "p99"), (0.001, "p99.9")):
        ax.axhline(q, color=_style.GRID, linewidth=0.8, zorder=0)
        ax.text(ax.get_xlim()[1], q, f" {lbl}", va="center", ha="left", color=_style.MUTED, fontsize=8.5)
    ax.legend(loc="lower left")
    fig.savefig(Path(out_dir) / "latency_tail.png")
    print(f"wrote {Path(out_dir) / 'latency_tail.png'}")


if __name__ == "__main__":
    main(*(sys.argv[1:4] if len(sys.argv) >= 4 else
           ("results/latency_samples.csv", "results/bench.json", "results")))
