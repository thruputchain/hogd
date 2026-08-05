#!/usr/bin/env python3
"""Plot the triggered layout converging and then stopping.

Two metrics, each with a trigger-off control. The control matters: without it the
rise could be the workload reorganising itself over 24 blocks rather than the
trigger doing it, and a ledger whose balances update in place has every reason to
look static. It stays flat, so the movement is the trigger's.

Both metrics are fractions of the same fast tier, but their reachable ranges are
not the same, and the figure marks that rather than hiding it. The access share
can reach 1.0. The byte share cannot: the workload only ever touches 11.5 MiB
(measured -- mean touched-byte fraction 0.0788 across all 73 occupied loglets)
against a 30 MiB tier, so its ceiling is 0.383. Drawn without that line the byte
series looks flat when it is in fact covering a good share of the room it has.

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

    def series(rows, idx):
        return [r[idx] for r in rows]

    # The first block where the trigger stops selecting anything. That is the
    # result: the stop condition was not designed, it fell out of the mixedness
    # threshold once no loglet was mixed any more.
    quiet = next((b for b, m, _, _ in on if m == 0), None)

    fig, (ax, bar) = plt.subplots(
        2, 1, figsize=(5.2, 3.6), height_ratios=(3.2, 1),
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

    # What the byte series can actually reach. Without it the reader compares a
    # line whose ceiling is 0.383 against one whose ceiling is 1.0.
    ax.axhline(BYTE_CEILING, color=SCATTERED, linewidth=1, dashes=(2, 3),
               alpha=0.7, zorder=2)
    ax.text(blocks[0] + 0.3, BYTE_CEILING + 0.012,
            f"most bytes the tier could ever serve ({BYTE_CEILING:.3f})",
            fontsize=7, color=SCATTERED, ha="left", va="bottom")

    for idx, color, name in (
        (3, CONSOLIDATED, "share of accesses"),
        (2, SCATTERED, "share of bytes touched"),
    ):
        ys = series(on, idx)
        ax.plot(blocks, ys, color=color, linewidth=2, marker="o",
                markersize=3.2, markeredgecolor=SURFACE, markeredgewidth=1.0,
                zorder=5)
        if off:
            # Same colour, muted and dashed: it is the same quantity, measured
            # with the trigger off, and pairing them by colour says so.
            ax.plot([b for b, _, _, _ in off], series(off, idx), color=color,
                    linewidth=1.3, dashes=(4, 2.5), alpha=0.5, zorder=3)
        ax.text(blocks[-1] + 0.4, ys[-1],
                f"  {name}\n  {ys[0]:.3f} → {ys[-1]:.3f}",
                fontsize=7.5, color=color, ha="left", va="center")

    if off:
        ax.text(blocks[-1] + 0.4, series(off, 3)[-1] - 0.055,
                "  dashed: trigger off", fontsize=7, color=INK_SOFT,
                ha="left", va="center")

    ax.set_ylabel("Fraction of the fast tier", fontsize=8.5, color=INK)
    ax.set_ylim(0, 1)
    ax.set_yticks([0, 0.25, 0.5, 0.75, 1.0])

    bar.bar(blocks, moved, color=INK_MUTED, width=0.62, zorder=3)
    bar.set_ylabel("Loglets\nrelocated", fontsize=8, color=INK, labelpad=2)
    bar.set_xlabel("Block boundary", fontsize=8.5, color=INK)
    bar.set_ylim(0, max(moved) + 0.6)
    bar.set_yticks(range(0, max(moved) + 1))
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
        a.set_xlim(blocks[0] - 0.7, blocks[-1] + 8.5)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"p2_convergence.{ext}", dpi=220, facecolor=SURFACE)

    acc, byt = series(on, 3), series(on, 2)
    print(f"accesses {acc[0]:.4f} -> {max(acc):.4f} "
          f"({(1 - acc[0]) / (1 - max(acc)):.2f}x fewer misses)")
    print(f"bytes    {byt[0]:.4f} -> {max(byt):.4f}, ceiling {BYTE_CEILING:.3f} "
          f"({byt[0] / BYTE_CEILING:.0%} -> {max(byt) / BYTE_CEILING:.0%} of it)")
    if off:
        oa = series(off, 3)
        print(f"control  {oa[0]:.4f} -> {oa[-1]:.4f} (trigger off)")
    print(f"trigger quiet from block {quiet}; {sum(moved)} loglets "
          f"= {sum(moved) * LOGLET_MIB} MiB relocated in total")


if __name__ == "__main__":
    main()
