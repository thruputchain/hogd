#!/usr/bin/env python3
"""Plot the V1 consolidation results.

(a) Modelled AMAT against access skew for the scattered layout, after one
    consolidation pass, and for a rank-sorted prefill.
(b) What the pass costs: AMAT achieved against the share of loglets relocated,
    as the mixedness selection threshold varies.

Reads data/v1_skew.csv and data/v1_mixedness.csv from run_v1.sh.
"""

import csv
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_x1 import (
    CONSOLIDATED, FAST_NS, INK, INK_MUTED, INK_SOFT, SCATTERED, SLOW_NS,
    SURFACE, style_axes,
)

HERE = pathlib.Path(__file__).parent
# Third categorical slot of the validated palette, for the reference arm.
CEILING = "#1baf7a"


def read_skew():
    arms = {}
    with open(HERE / "data" / "v1_skew.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            arms.setdefault(row["arm"], []).append(
                (float(row["theta"]), float(row["amat_ns"]))
            )
    return {k: sorted(v) for k, v in arms.items()}


def read_mixedness():
    rows = []
    with open(HERE / "data" / "v1_mixedness.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            rows.append((float(row["min_mixedness"]),
                         int(row["loglets_moved"]),
                         float(row["amat_ns"])))
    return rows


def plot_skew(ax, arms):
    for key, color, dash, name in (
        ("scattered", SCATTERED, (5, 2), "Scattered"),
        ("consolidated", CONSOLIDATED, None, "Consolidated"),
        ("ceiling", CEILING, (1, 2), "Rank-sorted prefill"),
    ):
        xs = [t for t, _ in arms[key]]
        ys = [a for _, a in arms[key]]
        ax.plot(xs, ys, color=color, linewidth=2,
                dashes=dash if dash else (None, None), marker="o",
                markersize=4.5, markeredgecolor=SURFACE, markeredgewidth=1.2,
                label=name, zorder=4)

    ax.axhline(FAST_NS, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
    ax.text(0.01, FAST_NS - 6, "DRAM", fontsize=7.5, color=INK_SOFT,
            ha="left", va="top")

    ax.set_xlim(-0.04, 1.03)
    ax.set_ylim(0, 200)
    ax.set_xlabel("Access skew (Zipf θ)", fontsize=8.5, color=INK)
    ax.set_ylabel("Modelled AMAT (ns)", fontsize=8.5, color=INK)
    ax.set_title("(a)", fontsize=9, color=INK, loc="left", pad=6)
    leg = ax.legend(fontsize=7.5, loc="lower left", frameon=False,
                    handlelength=2.2, borderpad=0.2, labelspacing=0.35,
                    bbox_to_anchor=(0.0, 0.02))
    for t in leg.get_texts():
        t.set_color(INK_SOFT)


def plot_cost(ax, rows, total_loglets=290):
    # Collapse to the distinct (work, result) operating points; the threshold
    # sweep lands on only a few, which is itself the finding.
    seen = {}
    for thr, moved, amat in rows:
        seen.setdefault((moved, amat), []).append(thr)
    points = sorted(seen.items())

    # Percent of the loglets that actually hold data, not of the whole heap:
    # the bench provisions 8x headroom so the heap is 30x the live set, which
    # would make any "fraction of heap" figure meaninglessly small.
    xs = [100.0 * m / total_loglets for (m, _), _ in points]
    ys = [a for (_, a), _ in points]
    ax.step(xs, ys, where="post", color=INK_MUTED, linewidth=1.5, zorder=2)
    ax.scatter(xs, ys, s=42, color=CONSOLIDATED, edgecolor=SURFACE,
               linewidth=1.2, zorder=4)

    for (moved, amat), thrs in points:
        x = 100.0 * moved / total_loglets
        label = f"θmix≥{min(thrs):.3f}" if moved else "no pass"
        ax.annotate(f"{label}\n{moved} loglets, {moved * 2} MiB", (x, amat),
                    textcoords="offset points",
                    xytext=(8, 12), fontsize=7.5,
                    color=INK_SOFT, ha="left")

    ax.axhline(FAST_NS, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
    ax.text(-6, FAST_NS - 6, "DRAM", fontsize=7.5, color=INK_SOFT,
            ha="left", va="top")

    ax.set_xlim(-8, 132)
    ax.set_ylim(0, 200)
    ax.set_xlabel("Occupied loglets relocated (%)", fontsize=8.5, color=INK)
    ax.set_ylabel("Modelled AMAT (ns)", fontsize=8.5, color=INK)
    ax.set_title("(b)", fontsize=9, color=INK, loc="left", pad=6)


def main():
    arms = read_skew()
    rows = read_mixedness()

    fig, axes = plt.subplots(1, 2, figsize=(7.4, 3.0), layout="constrained")
    fig.get_layout_engine().set(w_pad=0.10, wspace=0.06)
    fig.patch.set_facecolor(SURFACE)
    for ax in axes:
        style_axes(ax)
    plot_skew(axes[0], arms)
    plot_cost(axes[1], rows)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"v1_consolidation.{ext}", dpi=220,
                    facecolor=SURFACE)

    for key in ("scattered", "consolidated", "ceiling"):
        hi = dict(arms[key])
        print(f"{key:>20}: θ=0.99 {hi[0.99]:6.1f} ns   θ=0 {hi[0.0]:6.1f} ns")
    for (moved, amat), thrs in sorted(
            {(m, a): t for t, m, a in rows}.items()):
        print(f"  {moved:>4} loglets -> {amat:6.2f} ns")


if __name__ == "__main__":
    main()
