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
    blocks = [b for b, _, _, _ in on]
    moved = [m for _, m, _, _ in on]
    hits = [h for _, _, _, h in on]

    # The first block where the trigger stops selecting anything. That is the
    # result: the stop condition was not designed, it fell out of the mixedness
    # threshold once no loglet was mixed any more.
    quiet = next((b for b, m, _, _ in on if m == 0), None)

    fig, (ax, bar) = plt.subplots(
        2, 1, figsize=(4.8, 3.2), height_ratios=(3.2, 1),
        sharex=True, layout="constrained",
    )
    fig.patch.set_facecolor(SURFACE)
    for a in (ax, bar):
        style_axes(a)

    if quiet is not None:
        for a in (ax, bar):
            a.axvspan(quiet - 0.5, blocks[-1] + 0.5, color=CONSOLIDATED,
                      alpha=0.07, zorder=0, linewidth=0)

    if off:
        ys = [h for _, _, _, h in off]
        ax.plot([b for b, _, _, _ in off], ys, color=INK_MUTED, linewidth=1.4,
                dashes=(4, 2.5), zorder=3)
        ax.text(blocks[-1], ys[-1] - 0.008, "trigger off", fontsize=7.5,
                color=INK_SOFT, ha="right", va="top")

    ax.plot(blocks, hits, color=CONSOLIDATED, linewidth=2, marker="o",
            markersize=3.4, markeredgecolor=SURFACE, markeredgewidth=1.0,
            zorder=5)

    # Endpoints only. The climb is the shape; the two numbers that matter are
    # where it started and where it settled.
    ax.annotate(f"{hits[0]:.3f}", xy=(blocks[0], hits[0]), xytext=(3, -9),
                textcoords="offset points", fontsize=8, color=INK, ha="left")
    ax.annotate(f"{max(hits):.3f}", xy=(blocks[-1], hits[-1]), xytext=(-2, 7),
                textcoords="offset points", fontsize=8, color=INK, ha="right")
    if quiet is not None:
        # In the empty middle of the band, not down beside the control line.
        ax.text((quiet + blocks[-1]) / 2, (min(hits) + max(hits)) / 2,
                "trigger selects nothing", fontsize=7.5, color=INK_SOFT,
                ha="center", va="center")

    ax.set_ylabel("Fast-tier hit rate", fontsize=8.5, color=INK)
    ax.set_ylim(min(hits) - 0.03, max(hits) + 0.025)

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
    # Pin the ticks to the loglet ticks; the auto locator picks 0/5, which
    # correspond to nothing on the left-hand axis.
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
    print(f"bytes    {byt[0]:.4f} -> {max(byt):.4f}, ceiling {BYTE_CEILING:.3f} "
          f"({byt[0] / BYTE_CEILING:.0%} -> {max(byt) / BYTE_CEILING:.0%} of it) "
          f"[tabled, not plotted]")
    if off:
        oa = [h for _, _, _, h in off]
        print(f"control  {oa[0]:.4f} -> {oa[-1]:.4f} (trigger off)")
    print(f"trigger quiet from block {quiet}; {sum(moved)} loglets "
          f"= {sum(moved) * LOGLET_MIB} MiB relocated in total")


if __name__ == "__main__":
    main()
