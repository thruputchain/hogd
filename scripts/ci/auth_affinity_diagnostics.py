#!/usr/bin/env python3
"""Run the hog9 Auth placement comparison with the existing benchmark binary."""

from collections import Counter, defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


CPU_ROOT = Path("/sys/devices/system/cpu")
CASES = {
    f"verify_warm/batch:{batch}/parallelism:{parallelism}/real_time"
    for batch in (64, 512)
    for parallelism in (1, 2)
}


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def cpu_list(cpus):
    return ",".join(map(str, sorted(cpus)))


def topology(allowed):
    result = {}
    for cpu in sorted(allowed):
        directory = CPU_ROOT / f"cpu{cpu}"
        nodes = list(directory.glob("node[0-9]*"))
        caches = [entry for entry in (directory / "cache").glob("index*")
                  if read_optional(entry / "level") == "3"
                  and read_optional(entry / "type") == "Unified"]
        siblings = read_optional(directory / "topology/thread_siblings_list")
        if len(nodes) != 1 or len(caches) != 1 or not siblings:
            raise RuntimeError(f"CPU {cpu} lacks unambiguous NUMA/LLC/core topology")
        llc = read_optional(caches[0] / "shared_cpu_list")
        if not llc:
            raise RuntimeError(f"CPU {cpu} lacks a shared LLC mask")
        result[cpu] = {"node": nodes[0].name, "llc": llc, "siblings": siblings}
    return result


def choose_masks(cpus):
    by_node = defaultdict(lambda: defaultdict(list))
    seen_siblings = set()
    for cpu, details in sorted(cpus.items()):
        siblings = details["siblings"]
        if siblings not in seen_siblings:
            seen_siblings.add(siblings)
            by_node[details["node"]][details["llc"]].append(cpu)

    for width in range(8, 3, -1):
        for node, groups in sorted(by_node.items()):
            llcs = sorted(groups.values(), key=lambda cores: (-len(cores), cores))
            if len(llcs) < 2 or len(llcs[0]) < width:
                continue
            spread = []
            for rank in range(width):
                spread.extend(cores[rank] for cores in llcs if len(cores) > rank)
                if len(spread) >= width:
                    return node, set(llcs[0][:width]), set(spread[:width])
    raise RuntimeError("need 4–8 equal physical cores within one NUMA node, "
                       "with one mask in an LLC and the other across LLCs")


def pressure_snapshot(cpus):
    stat = Path("/proc/stat").read_text().splitlines()
    identifiers = {f"cpu{cpu}" for cpu in cpus}
    result = {
        "selected_cpu_stat": [line for line in stat if line.split()[0] in identifiers],
        "cpu_pressure": read_optional("/proc/pressure/cpu"),
        "cgroup": read_optional("/proc/self/cgroup"),
        "process_status": [
            line for line in Path("/proc/self/status").read_text().splitlines()
            if line.startswith(("Cpus_allowed_list:", "Mems_allowed_list:"))
        ],
    }
    # The qualified container normally sees its own cgroup as the mount root.
    # Record missing controls explicitly for kernels with different exposure.
    for path in ("cpu.stat", "cpu.pressure", "cpu.max",
                 "cpuset.cpus.effective", "cpuset.mems.effective",
                 "cpu/cpu.stat", "cpu/cpu.cfs_quota_us", "cpuset/cpuset.cpus"):
        result[path] = read_optional(Path("/sys/fs/cgroup") / path)
    return result


def validate_benchmark(output, expected_workers):
    data = json.loads(output.read_text())
    workers = int(data["context"]["auth_bench_cores"])
    if workers != expected_workers:
        raise RuntimeError(f"{output.name}: workers={workers}, wanted {expected_workers}")
    medians = {}
    for row in data["benchmarks"]:
        if (row.get("error_occurred") or row["run_name"] not in CASES
                or row.get("db_lookups_per_iter") != 0):
            raise RuntimeError(f"{output.name}: invalid warm benchmark result")
        if row.get("aggregate_name") == "median":
            counters = {
                key: row[key] for key in (
                    "txs_per_s", "cpu_us_per_tx", "txs_per_cpu_s", "cpus_busy"
                )
            }
            if row["repetitions"] != 3 or not all(
                math.isfinite(value) and value > 0 for value in counters.values()
            ):
                raise RuntimeError(f"{output.name}: incomplete warm median")
            medians[row["run_name"]] = counters
    if medians.keys() != CASES:
        raise RuntimeError(f"{output.name}: missing filtered benchmarks")
    return medians


def run_arm(binary, output, cpus, *, pin):
    command = [
        str(binary),
        "--auth_bench_batch_sizes=64,512",
        "--auth_bench_parallelism=1,2",
        "--benchmark_filter=^verify_warm/batch:(64|512)/parallelism:(1|2)/real_time$",
        "--benchmark_min_time=1s",
        "--benchmark_repetitions=3",
        "--benchmark_report_aggregates_only=true",
        f"--benchmark_out={output.with_suffix('.json')}",
        "--benchmark_out_format=json",
    ]
    if pin:
        command[:0] = ["taskset", "-c", cpu_list(cpus)]
    # The unpinned run inherits the normal CPU set and worker defaults.
    # Validate the worker count reported by the binary for every arm.
    proof = {"command": command, "allowed": sorted(cpus),
             "before": pressure_snapshot(cpus)}
    deadline = time.monotonic() + 150
    snapshots = Counter()
    try:
        with output.with_suffix(".log").open("w") as log:
            with subprocess.Popen(command, stdout=log,
                                  stderr=subprocess.STDOUT) as child:
                try:
                    # taskset execs the benchmark in place. Ignore its brief
                    # startup mask before exec and check every live bench thread.
                    while child.poll() is None:
                        if time.monotonic() >= deadline:
                            raise RuntimeError(f"{output.name}: benchmark timed out")
                        tasks = Path(f"/proc/{child.pid}/task")
                        masks = Counter()
                        try:
                            if Path(f"/proc/{child.pid}/exe").resolve(strict=True) == binary:
                                for task in tasks.iterdir():
                                    try:
                                        mask = os.sched_getaffinity(int(task.name))
                                    except ProcessLookupError:
                                        continue
                                    masks[cpu_list(mask)] += 1
                                    if not mask <= cpus:
                                        proof["escaped_mask"] = cpu_list(mask)
                                        raise RuntimeError(f"{output.name}: thread escaped its CPU mask")
                        except FileNotFoundError:
                            pass  # The process was exiting between procfs reads.
                        if masks:
                            signature = tuple(sorted(masks.items()))
                            snapshots[signature] += 1
                        time.sleep(0.1)
                    if child.returncode:
                        raise RuntimeError(f"{output.name}: benchmark exited {child.returncode}")
                finally:
                    if child.poll() is None:
                        child.kill()
    finally:
        proof["after"] = pressure_snapshot(cpus)
        proof["thread_mask_samples"] = [
            {"masks": dict(signature), "observations": count}
            for signature, count in snapshots.items()
        ]
        output.with_suffix(".proof.json").write_text(json.dumps(proof, indent=2) + "\n")
    if sum(snapshots.values()) < 10:
        raise RuntimeError(f"{output.name}: insufficient live affinity samples")
    # The recurring highest count rejects worker-count changes while allowing
    # a single transient initialization or teardown sample.
    stable_counts = [sum(count for _, count in signature)
                     for signature, observations in snapshots.items() if observations >= 3]
    if not stable_counts:
        raise RuntimeError(f"{output.name}: thread count did not stabilize")
    return max(stable_counts), validate_benchmark(output.with_suffix(".json"), len(cpus))


def main(binary, collection_output, dump_dir):
    binary = binary.resolve(strict=True)
    output = dump_dir / "auth-affinity"
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    allowed = os.sched_getaffinity(0)
    cpus = topology(allowed)
    with binary.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    metadata = {"sha256": digest, "host": os.getenv("HOG_HOST"), "topology": cpus}
    metadata_path = output / "topology.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    node, local, spread = choose_masks(cpus)
    metadata.update(node=node, local=sorted(local), spread=sorted(spread))
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    results = {}
    for name, mask in (("untuned", allowed), ("a-local", local),
                       ("b-spread", spread), ("b2-spread", spread), ("a2-local", local)):
        threads, medians = run_arm(binary, output / name, mask, pin=name != "untuned")
        results[name] = {"stable_threads": threads, "warm_medians": medians}
        print(f"Auth affinity {name}: {threads} threads, {medians}", flush=True)
    if len({results[name]["stable_threads"] for name in results if name != "untuned"}) != 1:
        raise RuntimeError("Auth placement arms had different recurring thread counts")
    (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    # The existing remote collector expects this file at its original path.
    shutil.copyfile(output / "untuned.json", collection_output / "auth_bench.json")


if __name__ == "__main__":
    try:
        main(*(Path(argument) for argument in sys.argv[1:]))
    except (KeyError, OSError, RuntimeError, ValueError) as error:
        sys.exit(f"Auth affinity diagnostic: {error}")
