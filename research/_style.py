"""Shared matplotlib styling for research figures: recessive axes and grid,
thin marks, a fixed categorical color order (never cycled)."""

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]  # slots 1-4, fixed order
INK = "#2b2b29"
MUTED = "#6f6e69"
GRID = "#e4e3de"


def apply() -> None:
    plt.rcParams.update({
        "figure.dpi": 140,
        "savefig.bbox": "tight",
        "font.size": 9.5,
        "axes.edgecolor": GRID,
        "axes.labelcolor": MUTED,
        "axes.titlecolor": INK,
        "axes.titlesize": 10.5,
        "axes.titleweight": "bold",
        "axes.titlelocation": "left",
        "axes.grid": True,
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "grid.color": GRID,
        "grid.linewidth": 0.6,
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "lines.linewidth": 2.0,
        "legend.frameon": False,
        "legend.labelcolor": INK,
    })
