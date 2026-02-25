#!/usr/bin/env python3
"""RCQ benchmark sweep: varies queue count across workload types and plots throughput.

Usage:
    python scripts/rcq_bench_sweep.py [options]

Options:
    --build-dir DIR   Path to build directory (required unless --plot-only)
    --blocks B        Blockchain blocks per run; n = B * queues * 1000 (default: 50)
    --workers W       Number of worker threads (default: 128)
    --queues Q [Q ..] Queue counts to sweep (default: 4 16 64 256 1024 4096)
    --output FILE     Output CSV file (default: rcq_bench_sweep.csv)
    --plot FILE       Output plot file (default: rcq_bench_sweep.png)
    --db-path PATH    Temp DB path (default: auto-created tempdir)

Using --blocks instead of a fixed entry count keeps the number of blockchain
commits constant across queue counts, so throughput reflects queue parallelism
rather than commit overhead.  With 50 blocks and 4096 queues the benchmark
targets roughly 10 seconds per workload at current hardware speeds.

Requires: matplotlib
Note: build hogdb_bench before running (make -C <build-dir> hogdb_bench)
"""

import argparse
import csv
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_BUILD_DIR = None
DEFAULT_BLOCKS = 50
# Entries per RCQ internal block (must match kRetrocausalQueueBlockEntriesMaxLen).
RCQ_ENTRIES_PER_BLOCK = 1000
DEFAULT_WORKERS = 128
DEFAULT_QUEUE_COUNTS = [4, 16, 64, 256, 1024, 4096]


def entries_for_queues(num_blocks, num_queues):
    """Compute total entries so that the benchmark runs exactly num_blocks blocks."""
    return num_blocks * num_queues * RCQ_ENTRIES_PER_BLOCK


def run_bench(bench_bin, db_path, cmd, n, workers, num_queues):
    shutil.rmtree(db_path, ignore_errors=True)
    Path(db_path).mkdir(parents=True, exist_ok=True)
    args = [
        bench_bin, db_path, cmd,
        "-n", str(n),
        "-w", str(workers),
        "-q", str(num_queues),
    ]
    print(f"  running: {' '.join(args)}", flush=True)
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAILED (exit {result.returncode}):", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return None
    return result.stdout


def parse_rcq(output):
    """Parse sequential rcq output.

    Returns (insert_eps, consume_eps, insert_wb, consume_wb) or None.
    """
    insert = re.search(r"insert: [\d.]+ seconds \(([\d.]+) entries/sec\)", output)
    consume = re.search(r"consume: [\d.]+ seconds \(([\d.]+) entries/sec\)", output)
    insert_wb = re.search(r"insert disk_bytes_written: (\d+)", output)
    consume_wb = re.search(r"consume disk_bytes_written: (\d+)", output)
    if insert and consume:
        return (
            float(insert.group(1)),
            float(consume.group(1)),
            int(insert_wb.group(1)) if insert_wb else None,
            int(consume_wb.group(1)) if consume_wb else None,
        )
    return None


def parse_rcq_concurrent(output):
    """Parse rcq_concurrent output.

    Returns (insert_eps, consume_eps, total_eps, total_wb) or None.
    """
    insert = re.search(
        r"concurrent insert: [\d.]+ seconds \(([\d.]+) entries/sec\)", output
    )
    consume = re.search(
        r"concurrent consume: [\d.]+ seconds \(([\d.]+) entries/sec\)", output
    )
    total = re.search(
        r"concurrent insert\+consume: [\d.]+ seconds \(([\d.]+) entries/sec\)", output
    )
    total_wb = re.search(r"concurrent disk_bytes_written: (\d+)", output)
    if insert and consume and total:
        return (
            float(insert.group(1)),
            float(consume.group(1)),
            float(total.group(1)),
            int(total_wb.group(1)) if total_wb else None,
        )
    return None


def main():
    parser = argparse.ArgumentParser(description="RCQ benchmark queue-count sweep")
    parser.add_argument(
        "--build-dir", default=DEFAULT_BUILD_DIR,
        help="Build directory (required unless --plot-only)"
    )
    parser.add_argument(
        "--blocks", "-b", type=int, default=DEFAULT_BLOCKS,
        help="Blockchain blocks per run (n = blocks * queues * 1000)"
    )
    parser.add_argument(
        "--workers", "-w", type=int, default=DEFAULT_WORKERS, help="Worker threads"
    )
    parser.add_argument(
        "--queues", "-q", type=int, nargs="+", default=DEFAULT_QUEUE_COUNTS,
        metavar="Q", help="Queue counts to sweep"
    )
    parser.add_argument(
        "--output", default="rcq_bench_sweep.csv", help="Output CSV file"
    )
    parser.add_argument(
        "--plot", default="rcq_bench_sweep.png", help="Output plot file"
    )
    parser.add_argument(
        "--db-path", default=None, help="Temp DB path (default: auto-created tempdir)"
    )
    parser.add_argument(
        "--plot-only", action="store_true",
        help="Skip benchmarks; read existing CSV and replot"
    )
    args = parser.parse_args()

    if args.plot_only:
        if not Path(args.output).exists():
            print(f"error: CSV not found at {args.output}", file=sys.stderr)
            sys.exit(1)
        rows = []
        with open(args.output, newline="") as f:
            for row in csv.DictReader(f):
                rows.append({
                    "workload": row["workload"],
                    "num_queues": int(row["num_queues"]),
                    "throughput_eps": float(row["throughput_eps"]),
                })
        # Infer queue list from CSV for x-axis ticks.
        seen = {}
        for row in rows:
            seen[row["num_queues"]] = True
        args.queues = sorted(seen.keys())
        print(f"Loaded {len(rows)} rows from {args.output}")
    else:
        if not args.build_dir:
            print("error: --build-dir is required (unless --plot-only)", file=sys.stderr)
            sys.exit(1)
        bench_bin = str(Path(args.build_dir) / "hogd/hogdb/hogdb_bench")
        if not Path(bench_bin).exists():
            print(f"error: hogdb_bench not found at {bench_bin}", file=sys.stderr)
            sys.exit(1)

    if not args.plot_only:
        rows = []  # list of dicts: workload, num_queues, throughput_eps

        # Use explicit --db-path or create a temporary directory.
        if args.db_path:
            db_path = args.db_path
            cleanup_db = True
        else:
            _tmpdir = tempfile.mkdtemp(prefix="rcq_bench_sweep_")
            db_path = _tmpdir
            cleanup_db = True

        try:
            for q in args.queues:
                n = entries_for_queues(args.blocks, q)
                print(f"\n=== {q} queue(s), n={n:,} entries ({args.blocks} blocks) ===")

                # Sequential: rcq
                output = run_bench(bench_bin, db_path, "rcq", n, args.workers, q)
                if output is not None:
                    parsed = parse_rcq(output)
                    if parsed:
                        insert_eps, consume_eps, insert_wb, consume_wb = parsed
                        rows.append({"workload": "rcq_insert", "num_queues": q, "throughput_eps": insert_eps})
                        rows.append({"workload": "rcq_consume", "num_queues": q, "throughput_eps": consume_eps})
                        if insert_wb is not None:
                            rows.append({"workload": "rcq_insert_wb", "num_queues": q, "throughput_eps": insert_wb})
                        if consume_wb is not None:
                            rows.append({"workload": "rcq_consume_wb", "num_queues": q, "throughput_eps": consume_wb})
                    else:
                        print(f"  warning: could not parse rcq output for q={q}", file=sys.stderr)

                # Concurrent: rcq_concurrent
                output = run_bench(bench_bin, db_path, "rcq_concurrent", n, args.workers, q)
                if output is not None:
                    parsed = parse_rcq_concurrent(output)
                    if parsed:
                        insert_eps, consume_eps, total_eps, total_wb = parsed
                        rows.append({"workload": "rcq_concurrent_insert", "num_queues": q, "throughput_eps": insert_eps})
                        rows.append({"workload": "rcq_concurrent_consume", "num_queues": q, "throughput_eps": consume_eps})
                        rows.append({"workload": "rcq_concurrent_total", "num_queues": q, "throughput_eps": total_eps})
                        if total_wb is not None:
                            rows.append({"workload": "rcq_concurrent_wb", "num_queues": q, "throughput_eps": total_wb})
                    else:
                        print(f"  warning: could not parse rcq_concurrent output for q={q}", file=sys.stderr)
        finally:
            if cleanup_db:
                shutil.rmtree(db_path, ignore_errors=True)

    if not rows:
        print("error: no results collected", file=sys.stderr)
        sys.exit(1)

    if not args.plot_only:
        # Write CSV
        with open(args.output, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["workload", "num_queues", "throughput_eps"])
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nCSV written to {args.output}")

    # Plot
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker

    # Color encodes mode (sequential vs concurrent); line style encodes operation
    # (insert vs consume).  rcq_concurrent_total is shown as a dotted reference.
    series = [
        # (workload_key,          label,                                  color,        linestyle, marker)
        # Sequential: timing wraps the full block loop including block_tx.commit()+db.commit()
        ("rcq_insert",            "seq insert (incl. commit)",            "steelblue",  "-",  "o"),
        ("rcq_consume",           "seq consume (incl. commit)",           "steelblue",  "--", "s"),
        # Concurrent: insert/consume timed only until last worker finishes InvokeTX;
        # total wraps the full block loop including block_tx.commit()+db.commit()
        ("rcq_concurrent_insert", "conc insert (excl. commit)",           "tomato",     "-",  "o"),
        ("rcq_concurrent_consume","conc consume (excl. commit)",          "tomato",     "--", "s"),
        ("rcq_concurrent_total",  "conc end-to-end (incl. commit)",       "tomato",     ":",  "^"),
    ]

    all_workloads = [s[0] for s in series]
    by_workload = {w: {"x": [], "y": []} for w in all_workloads}
    for row in rows:
        w = row["workload"]
        if w in by_workload:
            by_workload[w]["x"].append(row["num_queues"])
            by_workload[w]["y"].append(row["throughput_eps"])

    # Separate throughput rows from write-bytes rows.
    wb_workloads = {"rcq_insert_wb", "rcq_consume_wb", "rcq_concurrent_wb"}
    wb_series = [
        ("rcq_insert_wb",      "seq insert",          "steelblue", "-",  "o"),
        ("rcq_consume_wb",     "seq consume",         "steelblue", "--", "s"),
        ("rcq_concurrent_wb",  "conc end-to-end",     "tomato",    ":",  "^"),
    ]
    by_wb = {w[0]: {"x": [], "y": []} for w in wb_series}
    for row in rows:
        w = row["workload"]
        if w in by_wb:
            by_wb[w]["x"].append(row["num_queues"])
            by_wb[w]["y"].append(row["throughput_eps"])

    has_wb = any(by_wb[w[0]]["x"] for w in wb_series)

    if has_wb:
        fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    else:
        fig, ax = plt.subplots(figsize=(10, 5))
        ax2 = None

    for workload, label, color, ls, marker in series:
        d = by_workload[workload]
        if d["x"]:
            ax.plot(d["x"], d["y"], marker=marker, color=color,
                    linestyle=ls, label=label)

    # x-axis: log2 scale with integer tick labels at each measured queue count
    ax.set_xscale("log", base=2)
    ax.set_xticks(args.queues)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: str(int(v))))

    # y-axis: log10 scale
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(
        ticker.FuncFormatter(
            lambda v, _: f"{v/1e6:.1f}M" if v >= 1e6 else f"{v/1e3:.0f}K"
        )
    )

    ax.set_ylabel("Throughput (entries/sec, log\u2081\u2080 scale)")
    ax.set_title(f"RCQ Throughput vs Queue Count ({args.blocks} blocks/run)\n"
                 "seq and conc-total include block commit overhead; conc insert/consume exclude it")
    ax.legend()
    ax.grid(True, which="both", linestyle="--", alpha=0.5)

    if ax2 is not None:
        for workload, label, color, ls, marker in wb_series:
            d = by_wb[workload]
            if d["x"]:
                ax2.plot(d["x"], d["y"], marker=marker, color=color,
                         linestyle=ls, label=label)
        ax2.set_xscale("log", base=2)
        ax2.set_xticks(args.queues)
        ax2.xaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: str(int(v))))
        ax2.set_yscale("log")
        ax2.yaxis.set_major_formatter(
            ticker.FuncFormatter(
                lambda v, _: f"{v/1e9:.2f}GB" if v >= 1e9 else f"{v/1e6:.0f}MB"
            )
        )
        ax2.set_xlabel("Number of Queues (log\u2082 scale)")
        ax2.set_ylabel("WAL bytes written (log\u2081\u2080 scale)")
        ax2.set_title("ramdb WAL write I/O per run (hypothesis: conc inserts fewer bytes at low queue count)")
        ax2.legend()
        ax2.grid(True, which="both", linestyle="--", alpha=0.5)
    else:
        ax.set_xlabel("Number of Queues (log\u2082 scale)")

    plt.tight_layout()
    plt.savefig(args.plot, dpi=150)
    print(f"Plot written to {args.plot}")


if __name__ == "__main__":
    main()
