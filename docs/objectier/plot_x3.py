#!/usr/bin/env python3
"""Plot the X3 item-size sweep in the style of the ObjecTier paper's Figure 3.

Modelled AMAT against item size for both layouts. The paper's Figure 3 sweeps
skew on this same form; this sweeps object granularity, which Section 4.3
predicts matters and never measures.

Reads data/x3_size_sweep.csv as emitted by run_x3.sh.
"""

import csv
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedFormatter, FixedLocator, NullLocator

from plot_x1 import (
    CONSOLIDATED, FAST_NS, INK, INK_MUTED, INK_SOFT, SCATTERED, SLOW_NS,
    SURFACE, style_axes,
)

HERE = pathlib.Path(__file__).parent
LOGLET_BYTES = 2 * 1024 * 1024
HEADER_BYTES, KEY_BYTES = 24, 8


def items_per_loglet(value_size):
    return LOGLET_BYTES // (HEADER_BYTES + KEY_BYTES + value_size)


def read_sweep():
    rows = {}
    with open(HERE / "data" / "x3_size_sweep.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            rows.setdefault(row["layout"], []).append(
                (int(row["value_size"]), float(row["amat_ns"]))
            )
    return {k: sorted(v) for k, v in rows.items()}


def main():
    sweep = read_sweep()
    sizes = [s for s, _ in sweep["scattered"]]

    fig, ax = plt.subplots(figsize=(4.6, 3.0), layout="constrained")
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)

    # Tier floors sit behind the data, labelled in place rather than legended.
    # DRAM is labelled below its line -- the consolidated curve runs just above
    # it for the whole sweep.
    for y, name, dy, va in (
        (FAST_NS, "DRAM", -7, "top"), (SLOW_NS, "Slow tier", 8, "bottom")
    ):
        ax.axhline(y, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
        ax.text(
            sizes[0] * 0.92, y + dy, name, fontsize=7.5, color=INK_SOFT,
            ha="left", va=va,
        )

    for layout, color, dash, name in (
        ("scattered", SCATTERED, (5, 2), "Scattered"),
        ("consolidated", CONSOLIDATED, None, "Consolidated"),
    ):
        xs = [s for s, _ in sweep[layout]]
        ys = [a for _, a in sweep[layout]]
        ax.plot(
            xs, ys, color=color, linewidth=2,
            dashes=dash if dash else (None, None),
            marker="o", markersize=4.5, markeredgecolor=SURFACE,
            markeredgewidth=1.2, label=name, zorder=4,
        )

    # The gap is the result, so state it at both ends rather than making the
    # reader divide two curves by eye.
    for idx, va in ((0, "bottom"), (-1, "bottom")):
        size = sizes[idx]
        hi = dict(sweep["scattered"])[size]
        lo = dict(sweep["consolidated"])[size]
        ax.annotate(
            "", xy=(size, hi), xytext=(size, lo),
            arrowprops=dict(arrowstyle="<->", color=INK_SOFT, linewidth=0.9,
                            shrinkA=2, shrinkB=2),
        )
        ax.text(
            size * (1.12 if idx == 0 else 0.88), (hi + lo) / 2,
            f"{hi / lo:.2f}×", fontsize=8, color=INK,
            ha="left" if idx == 0 else "right", va="center",
        )

    ax.set_xscale("log", base=2)
    ax.set_xticks(sizes)
    ax.set_xticklabels([str(s) for s in sizes])
    ax.minorticks_off()
    ax.set_xlim(sizes[0] * 0.72, sizes[-1] * 1.38)
    ax.set_ylim(0, 340)
    ax.set_xlabel("Item value size (B)", fontsize=8.5, color=INK)
    ax.set_ylabel("Modelled AMAT (ns)", fontsize=8.5, color=INK)

    # A second labelling of the same axis -- not a second scale. Items per
    # tiering unit is the quantity the mechanism actually depends on.
    top = ax.secondary_xaxis("top")
    # A secondary log axis inherits the log locator/formatter, which would draw
    # decade labels straight through these; pin both explicitly.
    top.xaxis.set_major_locator(FixedLocator(sizes))
    top.xaxis.set_major_formatter(
        FixedFormatter([f"{items_per_loglet(s) / 1000:.1f}k" for s in sizes])
    )
    top.xaxis.set_minor_locator(NullLocator())
    top.set_xlabel("Items per 2 MiB loglet", fontsize=8.5, color=INK,
                   labelpad=4)
    top.tick_params(colors=INK_SOFT, labelsize=8, length=3, width=0.8)
    top.spines["top"].set_color("#e3e2de")
    top.spines["top"].set_linewidth(0.8)

    # The empty band between the scattered curve and the slow-tier line.
    leg = ax.legend(fontsize=7.5, loc="upper right", frameon=False,
                    handlelength=2.2, borderpad=0.2, labelspacing=0.35,
                    bbox_to_anchor=(0.99, 0.83))
    for text in leg.get_texts():
        text.set_color(INK_SOFT)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"x3_item_size.{ext}", dpi=220, facecolor=SURFACE)

    for size in sizes:
        hi = dict(sweep["scattered"])[size]
        lo = dict(sweep["consolidated"])[size]
        print(f"{size:>4} B  {items_per_loglet(size):>6} items/loglet  "
              f"{hi:6.1f} / {lo:5.1f} ns  ratio {hi / lo:.3f}")


if __name__ == "__main__":
    main()
