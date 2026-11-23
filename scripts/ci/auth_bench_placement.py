#!/usr/bin/env python3
"""Run selected cold Auth cases in the qualified CPU placement."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory


COLD_CASES = {
    f"verify_cold/batch:{batch}/txs_per_account:{txs}/real_time": batch // txs
    for batch in (64, 4096)
    for txs in (1, 4)
}
AGGREGATES = {"mean", "median", "stddev", "cv"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_topology():
    require(os.sched_getaffinity(0) == set(range(128)), "unexpected stock CPU mask")
    caches = set()
    siblings = set()
    nodes = set()
    for cpu in range(8):
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        cpu_nodes = list(path.glob("node[0-9]*"))
        cpu_caches = [entry for entry in (path / "cache").glob("index*")
                      if (entry / "level").read_text().strip() == "3"]
        require(len(cpu_nodes) == len(cpu_caches) == 1,
                f"CPU {cpu} lacks a unique NUMA node or last-level cache")
        [node], [llc] = cpu_nodes, cpu_caches
        nodes.add(node.name)
        caches.add((llc / "shared_cpu_list").read_text().strip())
        siblings.add((path / "topology/thread_siblings_list").read_text().strip())
    require(len(nodes) == len(caches) == 1 and len(siblings) == 8,
            "CPUs 0–7 must be eight physical cores in one LLC and NUMA node")


def collect_rows(report, expected, cores):
    context = report["context"]
    require(context["auth_bench_cores"] == str(cores) and
            context["library_build_type"] == "release", "unexpected Release benchmark context")
    cases = {}
    for row in report["benchmarks"]:
        case = row["run_name"]
        aggregate = row["aggregate_name"]
        require(case in expected and row["run_type"] == "aggregate" and
                aggregate in AGGREGATES and row["repetitions"] == 3 and
                not row.get("error_occurred") and not row.get("error_message") and
                not row.get("skipped"), f"unexpected or failed benchmark row: {case}")
        group = cases.setdefault(case, {})
        require(aggregate not in group, f"duplicate benchmark aggregate: {case} {aggregate}")
        if case in COLD_CASES and aggregate in {"mean", "median"}:
            require(row["accounts"] == row["db_lookups_per_iter"] == COLD_CASES[case],
                    f"unexpected account or lookup count: {case}")
        group[aggregate] = row
    require(set(cases) == expected and all(set(group) == AGGREGATES for group in cases.values()),
            "missing benchmark case or aggregate")
    return cases


def run(binary, destination, flags):
    try:
        check_topology()
    except (OSError, ValueError) as error:
        print(f"Auth benchmark placement unavailable: {error}; running stock benchmark",
              file=sys.stderr)
        subprocess.run([binary, *flags], check=True)
        return
    listing = subprocess.run([binary, "--benchmark_list_tests=true"], check=True,
                             stdout=subprocess.PIPE, text=True).stdout.splitlines()
    cases = set(listing)
    require(len(cases) == len(listing) and set(COLD_CASES) < cases,
            "invalid benchmark listing or missing selected cold cases")
    # Google Benchmark treats a leading '-' as inversion of the entire regex.
    selected = "^(" + "|".join(COLD_CASES) + ")$"
    flags = [flag for flag in flags if not flag.startswith("--benchmark_out=")]
    with TemporaryDirectory(dir=destination.parent) as work:
        directory = Path(work)
        stock_out, cold_out = directory / "stock.json", directory / "cold.json"
        subprocess.run([binary, *flags, f"--benchmark_filter=-{selected}",
                        f"--benchmark_out={stock_out}"], check=True)
        subprocess.run(["taskset", "-c", "0-7", binary, *flags, f"--benchmark_filter={selected}",
                        f"--benchmark_out={cold_out}"], check=True,
                       env={**os.environ, "DEFAULT_THREADPOOL_SIZE": "8"})
        stock = json.loads(stock_out.read_text())
        cold = json.loads(cold_out.read_text())
        rows = collect_rows(stock, cases - COLD_CASES.keys(), 128)
        rows.update(collect_rows(cold, set(COLD_CASES), 8))
        stock["benchmarks"] = [dict(row, family_index=index)
                               for index, case in enumerate(listing)
                               for row in rows[case].values()]
        stock["auth_bench_placement"] = {case: {"cpus": "0-7", "auth_bench_cores": 8}
                                         for case in COLD_CASES}
        stock_out.write_text(json.dumps(stock, indent=2) + "\n")
        stock_out.replace(destination)


if __name__ == "__main__":
    try:
        run(sys.argv[1], Path(sys.argv[2]), sys.argv[3:])
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"Auth benchmark placement: {error}")
