#!/usr/bin/env python3
"""Plot classifier aging against a hot set that moves.

The result is the shape of the un-aged line, not its height. It converges, the
hot set moves, and then it goes *flat* -- the sketch still believes the old
accounts are hot, so no loglet looks mixed, the trigger selects nothing, and the
layout stays broken while the system reports no work to do. A short enough aging
epoch recovers the full hit rate each time the hot set moves.

Reads data/p1_aging.csv as emitted by run_p2.sh.
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
REDRAW_EVERY = 15


def read_aging():
    arms = {}
    with open(HERE / "data" / "p1_aging.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            arms.setdefault(int(r["age_epoch"]), []).append(
                (int(r["block"]), float(r["hit_rate"]))
            )
    return {k: sorted(v) for k, v in sorted(arms.items())}


def blend(frac):
    """Interpolate scattered -> consolidated, so a shorter epoch reads warmer."""
    a = tuple(int(SCATTERED[i:i + 2], 16) for i in (1, 3, 5))
    b = tuple(int(CONSOLIDATED[i:i + 2], 16) for i in (1, 3, 5))
    return tuple((a[i] + (b[i] - a[i]) * frac) / 255 for i in range(3))


def main():
    arms = read_aging()
    epochs = list(arms)
    last = max(b for rows in arms.values() for b, _ in rows)

    fig, ax = plt.subplots(figsize=(5.0, 3.1), layout="constrained")
    fig.patch.set_facecolor(SURFACE)
    style_axes(ax)

    # Mark where the hot accounts change identity. Everything the classifier
    # learned before each of these lines is now about the wrong accounts.
    for x in range(REDRAW_EVERY, last + 1, REDRAW_EVERY):
        ax.axvline(x, color=INK_MUTED, linewidth=1, dashes=(3, 3), zorder=1)
        ax.text(x + 0.4, 0.906, "hot set moves", fontsize=7, color=INK_SOFT,
                ha="left", va="top", rotation=90)

    # Un-aged last so it sits on top: it is the line the figure is about.
    order = [e for e in epochs if e != 0] + ([0] if 0 in arms else [])
    for epoch in order:
        rows = arms[epoch]
        xs = [b for b, _ in rows]
        ys = [h for _, h in rows]
        if epoch == 0:
            color, width, dash, label = INK, 2.4, (5, 2), "never aged"
        else:
            # Shorter epoch -> warmer, so the sweep reads as one gradient.
            frac = 1 - (epochs.index(epoch) - 1) / max(len(epochs) - 2, 1)
            color, width, dash = blend(frac), 1.6, None
            label = f"every {epoch}"
        ax.plot(xs, ys, color=color, linewidth=width,
                dashes=dash if dash else (None, None), label=label, zorder=4)

    ax.set_xlabel("Block boundary", fontsize=8.5, color=INK)
    ax.set_ylabel("Fast-tier hit rate", fontsize=8.5, color=INK)
    ax.set_xlim(0, last)
    # Headroom above the plateau for a single-row legend, so it never sits on
    # the data -- the flat un-aged line is the whole point and must stay clear.
    ax.set_ylim(0.62, 0.98)

    leg = ax.legend(fontsize=7.5, loc="upper center", frameon=False,
                    handlelength=1.8, borderpad=0.2, labelspacing=0.25,
                    ncols=3, columnspacing=1.2,
                    title="sketch halved every N blocks", title_fontsize=7.5)
    leg.get_title().set_color(INK_SOFT)
    for text in leg.get_texts():
        text.set_color(INK_SOFT)

    figs = HERE / "figures"
    figs.mkdir(exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(figs / f"p1_aging.{ext}", dpi=220, facecolor=SURFACE)

    for epoch, rows in arms.items():
        hits = dict(rows)
        peak = max(h for b, h in rows if b < REDRAW_EVERY)
        name = "never aged" if epoch == 0 else f"halve/{epoch}"
        print(f"{name:>14}  pre-move peak {peak:.4f}  "
              f"final {hits[max(hits)]:.4f}")


if __name__ == "__main__":
    main()
