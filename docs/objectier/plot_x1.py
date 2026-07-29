#!/usr/bin/env python3
"""Plot the X1 layout analysis as the ObjecTier paper's Figure 2.

(a) CDF of per-loglet hot-byte utilization, scattered vs consolidated, with the
    fast-tier capacity cutoff. (b) Modelled AMAT for the two layouts against the
    fast- and slow-tier latency floors.

Reads data/x1_{scattered,consolidated}.csv as emitted by:
    ramdb_bench <db> layout_{scattered,consolidated} 50000000 64 0.99 32 3
"""

import csv
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import PathPatch
from matplotlib.path import Path

HERE = pathlib.Path(__file__).parent
DATA = HERE / "data"
FIGS = HERE / "figures"

# Categorical slots 1 and 2 of the validated default palette. Scattered is the
# baseline, consolidated the treatment. Ink tokens carry all text; the series
# colors stay on the marks.
SCATTERED = "#2a78d6"
CONSOLIDATED = "#eb6834"
INK = "#0b0b0b"
INK_SOFT = "#52514e"
INK_MUTED = "#8a8985"
GRID = "#e3e2de"
SURFACE = "#fcfcfb"

# Latency model and fast-tier share, mirrored from db_bench.cc.
FAST_NS, SLOW_NS = 70.0, 300.0
FAST_TIER_FRACTION = 0.20


def read_run(label):
    """Return (cdf_by_series, summary, amat) for one layout's CSV."""
    cdf = {}
    summary = amat = None
    with open(DATA / f"x1_{label}.csv", newline="") as fh:
        for row in csv.reader(fh):
            if row[0] == "layout_cdf":
                cdf.setdefault(row[2], ([], []))
                cdf[row[2]][0].append(float(row[4]))  # utilization
                cdf[row[2]][1].append(float(row[3]))  # cumulative fraction
            elif row[0] == "layout_summary":
                summary = {
                    "loglets": int(row[2]),
                    "items": int(row[3]),
                    "accesses": int(row[4]),
                    "mean_hot": float(row[5]),
                    "mean_live": float(row[6]),
                    "fast_hot": float(row[7]),
                }
            elif row[0] == "layout_amat":
                amat = {"hit": float(row[4]), "amat_ns": float(row[5])}
    return cdf, summary, amat


def style_axes(ax):
    """Recessive grid and axes; the data should be the only strong ink."""
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
        ax.spines[side].set_linewidth(0.8)
    ax.tick_params(colors=INK_SOFT, labelsize=8, length=3, width=0.8)


def rounded_bar(ax, x, height, width, color, radius_px=4):
    """A bar with a rounded data-end, anchored flat to the baseline.

    The corner radius is specified in pixels and converted separately for each
    axis, so the curve stays circular whatever the axes aspect ratio is. Doing
    this in data units instead yields wildly distorted caps.
    """
    bbox = ax.get_window_extent()
    xr = ax.get_xlim()[1] - ax.get_xlim()[0]
    yr = ax.get_ylim()[1] - ax.get_ylim()[0]
    rx = radius_px * xr / bbox.width
    ry = radius_px * yr / bbox.height
    rx, ry = min(rx, width / 2), min(ry, height / 2)

    left, right = x - width / 2, x + width / 2
    verts = [
        (left, 0.0), (left, height - ry),
        (left, height), (left + rx, height),      # rounded top-left
        (right - rx, height),
        (right, height), (right, height - ry),    # rounded top-right
        (right, 0.0), (left, 0.0),
    ]
    codes = [
        Path.MOVETO, Path.LINETO,
        Path.CURVE3, Path.CURVE3,
        Path.LINETO,
        Path.CURVE3, Path.CURVE3,
        Path.LINETO, Path.CLOSEPOLY,
    ]
    ax.add_patch(
        PathPatch(Path(verts, codes), facecolor=color, edgecolor="none",
                  zorder=3)
    )


def plot_cdf(ax, runs):
    for label, color, dash, name in (
        ("scattered", SCATTERED, (5, 2), "Scattered"),
        ("consolidated", CONSOLIDATED, None, "Consolidated"),
    ):
        xs, ys = runs[label][0]["hot"]
        ax.plot(
            xs, ys, color=color, linewidth=2,
            dashes=dash if dash else (None, None), label=name, zorder=4,
        )

    # Liveness is what ramdb's compactor already optimizes, and it is the same
    # under both layouts. Its CDF is a step at 1.0, which drawn as a curve reads
    # as a plot border -- so show it as the reference line it actually is. The
    # distance from it to the hotness curves is the whole argument.
    live = runs["scattered"][1]["mean_live"]
    ax.axvline(live, color=INK_MUTED, linewidth=1, dashes=(1, 2), zorder=2)
    ax.text(
        live - 0.02, 0.52, f"Live-byte utilization {live:.2f}",
        fontsize=7.5, color=INK_SOFT, ha="right", va="center", rotation=90,
    )

    cutoff = 1.0 - FAST_TIER_FRACTION
    ax.axhline(cutoff, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
    ax.text(
        0.02, cutoff + 0.02, "Fast tier capacity", fontsize=7.5,
        color=INK_SOFT, ha="left", va="bottom",
    )

    ax.set_xlim(0, 1.04)
    ax.set_ylim(0, 1.04)
    ax.set_xlabel("Hot-byte utilization", fontsize=8.5, color=INK)
    ax.set_ylabel("Cumulative fraction of loglets", fontsize=8.5, color=INK)
    ax.set_title("(a)", fontsize=9, color=INK, loc="left", pad=6)
    # Lower right is the only region both curves leave empty.
    leg = ax.legend(
        fontsize=7.5, loc="lower right", frameon=False, handlelength=2.2,
        borderpad=0.2, labelspacing=0.35, bbox_to_anchor=(0.87, 0.02),
    )
    for text in leg.get_texts():
        text.set_color(INK_SOFT)


def amat_bars(runs):
    return [
        ("Scattered", runs["scattered"][2]["amat_ns"], SCATTERED),
        ("Consolidated", runs["consolidated"][2]["amat_ns"], CONSOLIDATED),
    ]


def plot_amat_frame(ax, runs):
    """Everything except the bars: scales, tick labels, and the tier floors."""
    bars = amat_bars(runs)
    ax.set_xlim(-0.65, 1.65)
    ax.set_ylim(0, 340)
    ax.set_xticks(range(len(bars)))
    ax.set_xticklabels([b[0] for b in bars], fontsize=8.5, color=INK)
    ax.set_ylabel("Modelled AMAT (ns)", fontsize=8.5, color=INK)
    ax.set_title("(b)", fontsize=9, color=INK, loc="left", pad=6)

    # Tier floors first, so the bars sit over them; labels hug the left edge
    # where neither bar nor value label can reach them.
    for y, name in ((FAST_NS, "DRAM"), (SLOW_NS, "Slow tier")):
        ax.axhline(y, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
        ax.text(
            -0.6, y + 7, name, fontsize=7.5, color=INK_SOFT,
            ha="left", va="bottom",
        )


def plot_amat_bars(ax, runs):
    """The bars themselves. Must run after the layout is settled -- the corner
    radius is derived from the axes' pixel extent."""
    for i, (_, value, color) in enumerate(amat_bars(runs)):
        rounded_bar(ax, i, value, 0.5, color)
        ax.text(
            i, value + 10, f"{value:.0f} ns", ha="center", va="bottom",
            fontsize=8.5, color=INK,
        )


def main():
    runs = {label: read_run(label) for label in ("scattered", "consolidated")}
    FIGS.mkdir(exist_ok=True)

    fig, axes = plt.subplots(
        1, 2, figsize=(7.2, 2.9), width_ratios=[1.5, 1], layout="constrained"
    )
    fig.get_layout_engine().set(w_pad=0.10, wspace=0.06)
    fig.patch.set_facecolor(SURFACE)
    for ax in axes:
        style_axes(ax)
    plot_cdf(axes[0], runs)

    # rounded_bar reads the axes' pixel extent, so the layout has to be settled
    # before the bars are built -- and frozen afterwards so it stays that way.
    plot_amat_frame(axes[1], runs)
    fig.canvas.draw()
    fig.set_layout_engine("none")
    plot_amat_bars(axes[1], runs)

    for ext in ("pdf", "png"):
        fig.savefig(FIGS / f"x1_layout.{ext}", dpi=220, facecolor=SURFACE)

    # Print the numbers the figure encodes, so the caption can quote them.
    for label in ("scattered", "consolidated"):
        _, s, a = runs[label]
        print(
            f"{label:>13}: fast-tier hot util {s['fast_hot']:.3f}  "
            f"mean hot {s['mean_hot']:.3f}  mean live {s['mean_live']:.3f}  "
            f"hit {a['hit']:.3f}  AMAT {a['amat_ns']:.1f} ns"
        )
    ratio = runs["scattered"][2]["amat_ns"] / runs["consolidated"][2]["amat_ns"]
    print(f"AMAT ratio: {ratio:.2f}x")


if __name__ == "__main__":
    main()
