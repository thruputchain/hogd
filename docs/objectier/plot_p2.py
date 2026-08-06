#!/usr/bin/env python3
"""Plot the triggered layout converging and then stopping.

One metric, with a trigger-off control. The control matters: without it the rise
could be the workload reorganising itself over 24 blocks rather than the trigger
doing it, and a ledger whose balances update in place has every reason to look
static. It stays flat, so the movement is the trigger's.

The byte-share series is deliberately *not* here, though it is in the CSV. It
argues something else -- that hot-byte utilization is a poor proxy for the benefit
-- and carrying it cost a second line, a second control, and a ceiling annotation
without which it is actively misleading. Two numbers and a bound belong in a
table; see the P2.1 section of TODO.md.

The paper has no analogue for the time axis. Its results are static A/B
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


# Measured at -P 1.0: mean touched-byte fraction over every occupied loglet is
# 0.0788, so the workload touches 0.0788 * 73 * 2 MiB = 11.5 MiB in total. The
# byte share of a 15-loglet (30 MiB) tier therefore cannot exceed this.
BYTE_CEILING = 0.0788 * 73 * LOGLET_MIB / (15 * LOGLET_MIB)


def read_ceiling():
    """Rank-sorted prefill: the best layout available without a consolidator."""
    path = HERE / "data" / "p2_ceiling.csv"
    if not path.exists():
        return None
    with open(path, newline="") as fh:
        return float(next(csv.DictReader(fh))["hit_rate"])


def read_convergence():
    arms = {}
    with open(HERE / "data" / "p2_convergence.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            arms.setdefault(r["trigger"], []).append(
                (int(r["block"]), int(r["loglets_moved"]),
                 float(r["fast_hot_bytes"]), float(r["hit_rate"]))
            )
    return {k: sorted(v) for k, v in arms.items()}


def main():
    arms = read_convergence()
    on = arms["on"]
    off = arms.get("off", [])
    ceiling = read_ceiling()
    blocks = [b for b, _, _, _ in on]
    moved = [m for _, m, _, _ in on]
    hits = [h for _, _, _, h in on]
    floor = [h for _, _, _, h in off][0] if off else hits[0]

    # The first block where the trigger stops selecting anything. That is the
    # result: the stop condition was not designed, it fell out of the mixedness
    # threshold once no loglet was mixed any more.
    quiet = next((b for b, m, _, _ in on if m == 0), None)

    fig, (ax, bar) = plt.subplots(
        2, 1, figsize=(4.8, 3.4), height_ratios=(3.4, 1),
        sharex=True, layout="constrained",
    )
    fig.patch.set_facecolor(SURFACE)
    for a in (ax, bar):
        style_axes(a)

    if quiet is not None:
        for a in (ax, bar):
            a.axvspan(quiet, blocks[-1] + 1, color=CONSOLIDATED,
                      alpha=0.07, zorder=0, linewidth=0)
        # In the empty lower half, which the honest 0-1 axis leaves free. The
        # band would otherwise be an unexplained tint.
        ax.text((quiet + blocks[-1] + 1) / 2, 0.42, "trigger selects\nnothing",
                fontsize=7.5, color=INK_SOFT, ha="center", va="center")

    # The two bounds, so the climb is read against what is reachable rather than
    # against the top of the panel. A hit rate is a fraction, so the axis runs
    # 0 to 1: zooming to the data would make a 22-point rise fill the figure.
    # The ceiling is labelled above its line and the floor below theirs: only
    # 0.05 separates the ceiling from the plateau, so a label underneath it
    # would sit on the curve.
    for y, name, dy, va in ((floor, "trigger off", -0.022, "top"),
                            (ceiling, "rank-sorted prefill", 0.014, "bottom")):
        if y is None:
            continue
        ax.axhline(y, color=INK_MUTED, linewidth=1.1, dashes=(4, 2.5), zorder=2)
        ax.text(blocks[-1] + 1, y + dy, f"{name} ({y:.3f})", fontsize=7.5,
                color=INK_SOFT, ha="right", va=va)

    ax.plot(blocks, hits, color=CONSOLIDATED, linewidth=2, marker="o",
            markersize=3.2, markeredgecolor=SURFACE, markeredgewidth=1.0,
            zorder=5)
    ax.annotate(f"{max(hits):.3f}", xy=(blocks[-1], hits[-1]), xytext=(-4, -10),
                textcoords="offset points", fontsize=8, color=INK, ha="right")

    if ceiling is not None:
        share = (max(hits) - floor) / (ceiling - floor)
        ax.text(0.4, 0.06,
                f"{share:.0%} of the reachable gap, in {quiet} blocks",
                fontsize=7.5, color=INK, ha="left", va="bottom")

    ax.set_ylabel("Fast-tier hit rate", fontsize=8.5, color=INK)
    ax.set_ylim(0, 1)
    ax.set_yticks([0, 0.2, 0.4, 0.6, 0.8, 1.0])

    # Edge-aligned so block i's work occupies [i, i+0.62] and the axes can meet
    # at the origin; centred bars would put half of block 0's bar at x < 0.
    bar.bar(blocks, moved, color=INK_MUTED, width=0.62, align="edge", zorder=3)
    bar.set_ylabel("Loglets\nrelocated", fontsize=8, color=INK, labelpad=2)
    bar.set_xlabel("Block boundary", fontsize=8.5, color=INK)
    bar.set_ylim(0, max(moved) + 0.6)
    bar.set_yticks(range(0, max(moved) + 1))
    bar.set_xlim(0, blocks[-1] + 1)
    bar.set_xticks([b for b in blocks if b % 4 == 0] + [blocks[-1] + 1])

    # The same bars in bytes moved -- the cost figure an operator would budget.
    mib = bar.secondary_yaxis(
        "right", functions=(lambda v: v * LOGLET_MIB, lambda v: v / LOGLET_MIB)
    )
    mib.set_yticks([v * LOGLET_MIB for v in range(0, max(moved) + 1)])
    mib.set_ylabel("MiB", fontsize=8, color=INK, labelpad=2)
    mib.tick_params(colors=INK_SOFT, labelsize=8, length=3, width=0.8)
    mib.spines["right"].set_color("#e3e2de")
    mib.spines["right"].set_linewidth(0.8)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"p2_convergence.{ext}", dpi=220, facecolor=SURFACE)

    byt = [f for _, _, f, _ in on]
    print(f"accesses {hits[0]:.4f} -> {max(hits):.4f} "
          f"({(1 - hits[0]) / (1 - max(hits)):.2f}x fewer misses)")
    if ceiling is not None:
        print(f"floor {floor:.4f}  ceiling {ceiling:.4f}  "
              f"captured {(max(hits) - floor) / (ceiling - floor):.1%}")
    print(f"bytes    {byt[0]:.4f} -> {max(byt):.4f}, ceiling {BYTE_CEILING:.3f} "
          f"[tabled, not plotted]")
    print(f"trigger quiet from block {quiet}; {sum(moved)} loglets "
          f"= {sum(moved) * LOGLET_MIB} MiB relocated in total")


if __name__ == "__main__":
    main()
