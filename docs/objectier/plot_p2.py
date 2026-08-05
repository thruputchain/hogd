#!/usr/bin/env python3
"""Plot the triggered layout converging and then stopping.

The ObjecTier paper has no analogue for this figure. Its own results are static
A/B comparisons of a pre-arranged layout, and Section 6 raises the question this
answers -- whether a consolidator reaches a steady state or keeps fighting new
writes -- without testing it.

Reads data/p2_convergence.csv as emitted by run_p2.sh.
"""

import csv
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_x1 import (
    CONSOLIDATED, INK, INK_MUTED, INK_SOFT, SCATTERED, SURFACE, style_axes,
)

HERE = pathlib.Path(__file__).parent


def read_convergence():
    with open(HERE / "data" / "p2_convergence.csv", newline="") as fh:
        return [
            (int(r["block"]), int(r["loglets_moved"]), float(r["hit_rate"]))
            for r in csv.DictReader(fh)
        ]


def main():
    rows = read_convergence()
    blocks = [b for b, _, _ in rows]
    hits = [h for _, _, h in rows]
    moved = [m for _, m, _ in rows]
    start, plateau = hits[0], max(hits)

    # The first block where the trigger stops selecting anything. That is the
    # result: the stop condition was not designed, it fell out of the mixedness
    # threshold once no loglet was mixed any more.
    quiet = next((b for b, m, _ in rows if m == 0), None)

    fig, (ax, bar) = plt.subplots(
        2, 1, figsize=(4.6, 3.2), height_ratios=(3.4, 1),
        sharex=True, layout="constrained",
    )
    fig.patch.set_facecolor(SURFACE)
    for a in (ax, bar):
        style_axes(a)

    # Where it started, so the climb is read against the scattered layout rather
    # than against the axis.
    ax.axhline(start, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
    ax.text(blocks[-1], start - 0.012, "scattered start", fontsize=7.5,
            color=INK_SOFT, ha="right", va="top")

    if quiet is not None:
        ax.axvspan(quiet - 0.5, blocks[-1] + 0.5, color=CONSOLIDATED,
                   alpha=0.07, zorder=0, linewidth=0)
        ax.text((quiet + blocks[-1]) / 2, start + 0.008,
                "trigger selects nothing", fontsize=7.5, color=INK_SOFT,
                ha="center", va="bottom")

    ax.plot(blocks, hits, color=CONSOLIDATED, linewidth=2, marker="o",
            markersize=4, markeredgecolor=SURFACE, markeredgewidth=1.1,
            zorder=4)

    ax.annotate(
        f"{plateau:.4f}", xy=(blocks[-1], plateau), xytext=(-4, 6),
        textcoords="offset points", fontsize=8, color=INK, ha="right",
    )
    ax.set_ylabel("Fast-tier hit rate", fontsize=8.5, color=INK)
    ax.set_ylim(start - 0.045, plateau + 0.035)

    # Work per block, on its own axis: the point is that it goes to zero and
    # stays there, which a shared axis would flatten into the hit-rate line.
    bar.bar(blocks, moved, color=SCATTERED, width=0.62, zorder=3)
    bar.set_ylabel("Loglets\nrelocated", fontsize=8, color=INK, labelpad=2)
    bar.set_xlabel("Block boundary", fontsize=8.5, color=INK)
    bar.set_ylim(0, max(moved) + 0.6)
    bar.set_yticks(range(0, max(moved) + 1))
    bar.set_xlim(blocks[0] - 0.7, blocks[-1] + 0.7)
    bar.set_xticks([b for b in blocks if b % 2 == 0])

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"p2_convergence.{ext}", dpi=220, facecolor=SURFACE)

    print(f"start {start:.4f} -> plateau {plateau:.4f}"
          f" ({(1 - start) / (1 - plateau):.2f}x fewer misses)")
    print(f"trigger quiet from block {quiet}; "
          f"{sum(moved)} loglets relocated in total")


if __name__ == "__main__":
    main()
