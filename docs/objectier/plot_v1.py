#!/usr/bin/env python3
"""Plot the V1 consolidation results.

The measured quantity is the fast-tier hit rate: the share of accesses landing in
the hottest 20% of loglets, computed from each item's real loglet and an
independent access trace. Nothing here times memory -- this host has one NUMA
node and no slow tier. AMAT is a one-line model on top of the hit rate,
AMAT = hit*fast + (1-hit)*slow, so it carries no information the hit rate does
not, and its absolute values depend entirely on the assumed latencies. Panel (c)
plots that dependence rather than burying it in a constant.

(a) Hit rate against access skew, for the scattered layout, after consolidation,
    and for a rank-sorted prefill.
(b) What the pass costs: hit rate against the share of occupied loglets moved.
(c) Access concentration: cumulative share of accesses against share of loglets,
    hottest first. The hit rate at *any* fast-tier capacity is a point on this
    curve, so the 20% used elsewhere is only where we read it off.
(d) How much the headline speedup depends on the assumed slow-tier latency.

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

# Loglets holding live data. The heap is ~9062 loglets because the bench
# provisions 8x headroom to keep the compactor idle, so quoting cost against the
# heap would understate it 30x for reasons unrelated to the work done.
OCCUPIED = 290

# Fast-tier capacity, as a share of occupied loglets. The paper's oracle picks the
# top 20% of pages (50 GB DRAM against 200 GB Optane); matching it keeps the
# comparison aligned. Panel (c) shows how much this choice matters.
FAST_TIER_FRACTION = 0.20


def amat(hit, slow=SLOW_NS, fast=FAST_NS):
    return (hit * fast) + ((1.0 - hit) * slow)


def read_skew():
    arms = {}
    with open(HERE / "data" / "v1_skew.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            arms.setdefault(row["arm"], []).append(
                (float(row["theta"]), float(row["hit"]))
            )
    return {k: sorted(v) for k, v in arms.items()}


def read_capacity():
    arms = {}
    with open(HERE / "data" / "v1_capacity.csv", newline="") as fh:
        for row in csv.reader(fh):
            arms.setdefault(row[1], []).append((float(row[2]), float(row[3])))
    return {k: sorted(v) for k, v in arms.items()}


def plot_capacity(ax, arms):
    for key, color, dash, name in (
        ("zipf_payments", SCATTERED, (5, 2), "Scattered"),
        ("zipf_payments+consolidated", CONSOLIDATED, None, "Consolidated"),
    ):
        xs = [100.0 * x for x, _ in arms[key]]
        ys = [y for _, y in arms[key]]
        ax.plot(xs, ys, color=color, linewidth=2,
                dashes=dash if dash else (None, None), label=name, zorder=4)

    ax.axvline(100.0 * FAST_TIER_FRACTION, color=INK_MUTED, linewidth=1,
               dashes=(4, 3), zorder=1)
    ax.text(100.0 * FAST_TIER_FRACTION + 2, 0.06,
            f"{100 * FAST_TIER_FRACTION:.0f}% — where\nevery other panel reads",
            fontsize=7.5, color=INK_SOFT, ha="left", va="bottom")

    ax.set_xlim(0, 100)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("Fast-tier capacity (% of occupied loglets)", fontsize=8.5,
                  color=INK)
    ax.set_ylabel("Fast-tier hit rate (measured)", fontsize=8.5, color=INK)
    ax.set_title("(c)", fontsize=9, color=INK, loc="left", pad=6)
    leg = ax.legend(fontsize=7.5, loc="lower right", frameon=False,
                    handlelength=2.2, borderpad=0.2, labelspacing=0.35)
    for t in leg.get_texts():
        t.set_color(INK_SOFT)


def read_mixedness():
    seen = {}
    with open(HERE / "data" / "v1_mixedness.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            key = (int(row["loglets_moved"]), float(row["hit"]))
            seen.setdefault(key, []).append(float(row["min_mixedness"]))
    return sorted(seen.items())


def add_amat_axis(ax):
    """Label the same axis in modelled nanoseconds.

    A deterministic unit conversion of one measure, not a second scale for a
    second series -- the same relabelling idea as the items-per-loglet axis in
    plot_x3.
    """
    sec = ax.secondary_yaxis(
        "right",
        functions=(amat,
                   lambda a: (SLOW_NS - a) / (SLOW_NS - FAST_NS)),
    )
    sec.set_ylabel(f"Modelled AMAT (ns), {FAST_NS:.0f}/{SLOW_NS:.0f}",
                   fontsize=8, color=INK_SOFT)
    sec.tick_params(colors=INK_SOFT, labelsize=8, length=3, width=0.8)
    sec.spines["right"].set_color("#e3e2de")
    sec.spines["right"].set_linewidth(0.8)


def plot_skew(ax, arms):
    for key, color, dash, name in (
        ("scattered", SCATTERED, (5, 2), "Scattered"),
        ("consolidated", CONSOLIDATED, None, "Consolidated"),
        ("ceiling", CEILING, (1, 2), "Rank-sorted prefill"),
    ):
        xs = [t for t, _ in arms[key]]
        ys = [h for _, h in arms[key]]
        ax.plot(xs, ys, color=color, linewidth=2,
                dashes=dash if dash else (None, None), marker="o",
                markersize=4.5, markeredgecolor=SURFACE, markeredgewidth=1.2,
                label=name, zorder=4)

    ax.set_xlim(0, 1.03)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("Access skew (Zipf θ)", fontsize=8.5, color=INK)
    ax.set_ylabel("Fast-tier hit rate (measured)", fontsize=8.5, color=INK)
    ax.set_title("(a)", fontsize=9, color=INK, loc="left", pad=6)
    leg = ax.legend(fontsize=7.5, loc="upper left", frameon=False,
                    handlelength=2.2, borderpad=0.2, labelspacing=0.35)
    for t in leg.get_texts():
        t.set_color(INK_SOFT)


def plot_cost(ax, points):
    xs = [100.0 * m / OCCUPIED for (m, _), _ in points]
    ys = [h for (_, h), _ in points]
    ax.step(xs, ys, where="post", color=INK_MUTED, linewidth=1.5, zorder=2)
    ax.scatter(xs, ys, s=42, color=CONSOLIDATED, edgecolor=SURFACE,
               linewidth=1.2, zorder=4)

    # The two relocating points share a y, so stagger their labels vertically.
    offsets = {0: (9, -14), 107: (8, -16), 289: (6, 14)}
    for (moved, hit), thrs in points:
        x = 100.0 * moved / OCCUPIED
        head = f"θmix≥{min(thrs):.3f}" if moved else "no pass"
        ax.annotate(f"{head}\n{moved} loglets, {moved * 2} MiB",
                    (x, hit), textcoords="offset points",
                    xytext=offsets.get(moved, (9, -6)),
                    fontsize=7.5, color=INK_SOFT, ha="left", va="center")

    ax.set_xlim(0, 142)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("Occupied loglets relocated (%)", fontsize=8.5, color=INK)
    ax.set_ylabel("Fast-tier hit rate (measured)", fontsize=8.5, color=INK)
    ax.set_title("(b)", fontsize=9, color=INK, loc="left", pad=6)


def plot_sensitivity(ax, hit_scattered, hit_consolidated):
    slows = [FAST_NS + (i * 5.0) for i in range(2, 200)]
    ratios = [amat(hit_scattered, s) / amat(hit_consolidated, s) for s in slows]
    ax.plot(slows, ratios, color=CONSOLIDATED, linewidth=2, zorder=4)

    # The value quoted everywhere else, and what it is contingent on.
    r300 = amat(hit_scattered, SLOW_NS) / amat(hit_consolidated, SLOW_NS)
    ax.scatter([SLOW_NS], [r300], s=52, color=CONSOLIDATED,
               edgecolor=SURFACE, linewidth=1.4, zorder=5)
    ax.annotate(f"{r300:.2f}× at {SLOW_NS:.0f} ns\n(the quoted figure)",
                (SLOW_NS, r300), textcoords="offset points", xytext=(12, -6),
                fontsize=7.5, color=INK, ha="left", va="top")

    # The assumption-free statement: proportionally fewer slow-tier accesses.
    invariant = (1 - hit_scattered) / (1 - hit_consolidated)
    ax.axhline(invariant, color=INK_MUTED, linewidth=1, dashes=(4, 3), zorder=1)
    ax.text(30, invariant + 0.05,
            f"{invariant:.2f}× fewer slow-tier accesses\n(no latency assumed)",
            fontsize=7.5, color=INK_SOFT, ha="left", va="bottom")

    ax.axhline(1.0, color=INK_MUTED, linewidth=1, dashes=(2, 3), zorder=1)
    ax.text(30, 1.06, "no change", fontsize=7.5, color=INK_SOFT,
            ha="left", va="bottom")

    ax.set_xlim(0, 1060)
    ax.set_ylim(0, 3.3)
    ax.set_xlabel("Assumed slow-tier latency (ns)", fontsize=8.5, color=INK)
    ax.set_ylabel("Modelled speedup", fontsize=8.5, color=INK)
    ax.set_title("(d)", fontsize=9, color=INK, loc="left", pad=6)


def main():
    arms = read_skew()
    points = read_mixedness()

    caps = read_capacity()

    fig, axes = plt.subplots(2, 2, figsize=(7.6, 6.0), layout="constrained")
    fig.get_layout_engine().set(w_pad=0.12, h_pad=0.12, wspace=0.08,
                                hspace=0.08)
    fig.patch.set_facecolor(SURFACE)
    flat = axes.flatten()
    for ax in flat:
        style_axes(ax)

    plot_skew(flat[0], arms)
    add_amat_axis(flat[0])
    plot_cost(flat[1], points)
    plot_capacity(flat[2], caps)

    hs = dict(arms["scattered"])[0.99]
    hc = dict(arms["consolidated"])[0.99]
    plot_sensitivity(flat[3], hs, hc)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"v1_consolidation.{ext}", dpi=220,
                    facecolor=SURFACE)

    print(f"measured at θ=0.99: hit {hs:.4f} -> {hc:.4f} "
          f"(+{100 * (hc - hs):.1f} pp, "
          f"{(1 - hs) / (1 - hc):.2f}x fewer slow-tier accesses)")
    for slow in (150, 300, 500, 1000):
        print(f"  modelled speedup at {slow:>4} ns slow tier: "
              f"{amat(hs, slow) / amat(hc, slow):.2f}x")


if __name__ == "__main__":
    main()
