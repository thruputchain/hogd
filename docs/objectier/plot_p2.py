#!/usr/bin/env python3
"""Plot the triggered layout converging and then stopping.

Two metrics on one axis, deliberately. Both are fractions in [0, 1] of the same
fast tier, so they are directly comparable, and the comparison is a result: the
share of *accesses* served by the tier climbs 20 points while the share of *bytes*
in it that anyone touches barely moves. The byte measure is the ObjecTier paper's
own Figure 2a metric, and on this workload it under-reports the benefit almost
entirely -- consolidation is not packing the tier fuller, it is packing the right
things into it.

The paper has no analogue for the time axis either. Its results are static A/B
comparisons of a pre-arranged layout, and Section 6 raises the question this
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
LOGLET_MIB = 2


def read_convergence():
    with open(HERE / "data" / "p2_convergence.csv", newline="") as fh:
        return [
            (int(r["block"]), int(r["loglets_moved"]),
             float(r["fast_hot_bytes"]), float(r["hit_rate"]))
            for r in csv.DictReader(fh)
        ]


def main():
    rows = read_convergence()
    blocks = [b for b, _, _, _ in rows]
    moved = [m for _, m, _, _ in rows]
    series = {
        "accesses": [h for _, _, _, h in rows],
        "bytes": [f for _, _, f, _ in rows],
    }

    # The first block where the trigger stops selecting anything. That is the
    # result: the stop condition was not designed, it fell out of the mixedness
    # threshold once no loglet was mixed any more.
    quiet = next((b for b, m, _, _ in rows if m == 0), None)

    fig, (ax, bar) = plt.subplots(
        2, 1, figsize=(5.0, 3.5), height_ratios=(3.2, 1),
        sharex=True, layout="constrained",
    )
    fig.patch.set_facecolor(SURFACE)
    for a in (ax, bar):
        style_axes(a)

    if quiet is not None:
        for a in (ax, bar):
            a.axvspan(quiet - 0.5, blocks[-1] + 0.5, color=CONSOLIDATED,
                      alpha=0.07, zorder=0, linewidth=0)
        ax.text((quiet + blocks[-1]) / 2, 0.975, "trigger selects nothing",
                fontsize=7.5, color=INK_SOFT, ha="center", va="top")

    for key, color, name in (
        ("accesses", CONSOLIDATED, "share of accesses"),
        ("bytes", SCATTERED, "share of bytes touched"),
    ):
        ys = series[key]
        ax.plot(blocks, ys, color=color, linewidth=2, marker="o",
                markersize=3.4, markeredgecolor=SURFACE, markeredgewidth=1.0,
                zorder=4)
        # Labelled in place rather than legended: with two lines this far apart
        # a legend only adds a lookup.
        ax.text(blocks[-1] + 0.4, ys[-1], f"  {name}\n  {ys[0]:.3f} → {ys[-1]:.3f}",
                fontsize=7.5, color=color, ha="left", va="center")

    # State both deltas, since the gap between them is the point and dividing two
    # curves by eye is exactly what the reader should not have to do.
    d_acc = series["accesses"][-1] - series["accesses"][0]
    d_byt = series["bytes"][-1] - series["bytes"][0]
    ax.text(0.4, 0.05,
            f"+{d_acc:.3f} accesses vs +{d_byt:.3f} bytes",
            fontsize=7.5, color=INK, ha="left", va="bottom")

    ax.set_ylabel("Fraction of the fast tier", fontsize=8.5, color=INK)
    ax.set_ylim(0, 1)
    ax.set_yticks([0, 0.25, 0.5, 0.75, 1.0])

    bar.bar(blocks, moved, color=INK_MUTED, width=0.62, zorder=3)
    bar.set_ylabel("Loglets\nrelocated", fontsize=8, color=INK, labelpad=2)
    bar.set_xlabel("Block boundary", fontsize=8.5, color=INK)
    bar.set_ylim(0, max(moved) + 0.6)
    bar.set_yticks(range(0, max(moved) + 1))
    bar.set_xlim(blocks[0] - 0.7, blocks[-1] + 0.7)
    bar.set_xticks([b for b in blocks if b % 4 == 0])

    # The same bars in bytes moved -- the cost figure an operator would budget.
    mib = bar.secondary_yaxis(
        "right", functions=(lambda v: v * LOGLET_MIB, lambda v: v / LOGLET_MIB)
    )
    mib.set_ylabel("MiB", fontsize=8, color=INK, labelpad=2)
    mib.tick_params(colors=INK_SOFT, labelsize=8, length=3, width=0.8)
    mib.spines["right"].set_color("#e3e2de")
    mib.spines["right"].set_linewidth(0.8)

    # Room on the right for the in-place line labels.
    for a in (ax, bar):
        a.set_xlim(blocks[0] - 0.7, blocks[-1] + 7.5)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"p2_convergence.{ext}", dpi=220, facecolor=SURFACE)

    acc, byt = series["accesses"], series["bytes"]
    print(f"accesses {acc[0]:.4f} -> {max(acc):.4f} "
          f"({(1 - acc[0]) / (1 - max(acc)):.2f}x fewer misses)")
    print(f"bytes    {byt[0]:.4f} -> {max(byt):.4f} "
          f"(+{max(byt) - byt[0]:.4f}, {(max(byt) / byt[0] - 1) * 100:.0f}% relative)")
    print(f"trigger quiet from block {quiet}; {sum(moved)} loglets "
          f"= {sum(moved) * LOGLET_MIB} MiB relocated in total")


if __name__ == "__main__":
    main()
