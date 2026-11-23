#!/usr/bin/env python3
"""Compare cold Auth cases with stock and restricted worker capacity."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from statistics import median
import subprocess
import sys
import time

CASE = "verify_cold/batch:64/txs_per_account:1/real_time"
ADAPTIVE_CASES = {f"verify_cold/batch:{batch}/txs_per_account:{txs}/real_time": batch // txs
                  for batch in (64, 4096) for txs in (1, 4)}
ADAPTIVE_ORDER = ("stock", "mask8-pool8", "mask8-pool8", "stock")
ORDER = ("stock", "pool8", "mask8-pool8", "mask8-pool8", "pool8", "stock")


def require(valid, message):
    if not valid:
        raise ValueError(message)


def topology():
    require(os.sched_getaffinity(0) == set(range(128)), "expected the stock 128 CPUs")
    root = Path("/sys/devices/system/cpu")
    rows = {}
    for cpu in range(128):
        path = root / f"cpu{cpu}"
        [node] = [entry.name for entry in path.glob("node[0-9]*")]
        [llc] = [entry for entry in (path / "cache").glob("index*")
                 if (entry / "level").read_text().strip() == "3"]
        rows[cpu] = {"node": node, "llc": (llc / "shared_cpu_list").read_text().strip(),
                     "siblings": (path / "topology/thread_siblings_list").read_text().strip()}
    narrow = [rows[cpu] for cpu in range(8)]
    require(len({row["siblings"] for row in narrow}) == 8 and
            len({(row["node"], row["llc"]) for row in narrow}) == 1,
            "CPUs 0–7 must be eight physical cores in the same LLC and NUMA node")
    return rows


def run_arm(binary, output, arm, cases, min_time):
    pool = 128 if arm == "stock" else 8
    mask = "0-7" if arm == "mask8-pool8" else "0-127"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("DEFAULT_THREADPOOL_SIZE") and
           key not in {"AUTH_BENCH_PIN_WARM_BATCH64", "HOGD_AUTH_BENCH_POST_INIT_CPUS"}}
    if arm != "stock":
        env["DEFAULT_THREADPOOL_SIZE"] = "8"
    case_pattern = "|".join(cases)
    command = [str(binary), f"--benchmark_filter=^({case_pattern})$",
               f"--benchmark_min_time={min_time}",
               "--benchmark_repetitions=3", "--benchmark_out_format=json",
               f"--benchmark_out={output / 'result.json'}"]
    if arm == "mask8-pool8":
        command = ["taskset", "-c", mask, *command]
    workers = []
    with (output / "bench.log").open("w") as log:
        with subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT) as child:
            try:
                deadline = time.monotonic() + 120
                while child.poll() is None and time.monotonic() < deadline:
                    if "CPU Caches:" in (output / "bench.log").read_text(errors="replace"):
                        for task in sorted(Path(f"/proc/{child.pid}/task").iterdir()):
                            status = (task / "status").read_text()
                            [allowed] = re.findall(r"^Cpus_allowed_list:\s+(\S+)$", status, re.M)
                            workers.append({"tid": int(task.name), "allowed": allowed})
                        break
                    time.sleep(.05)
                require(child.wait(timeout=max(1, deadline-time.monotonic())) == 0,
                        f"{arm} exited unsuccessfully")
            finally:
                if child.poll() is None:
                    child.kill()
    proof = {"arm": arm, "mask": mask, "pool_setting": env.get("DEFAULT_THREADPOOL_SIZE"),
             "workers": workers}
    (output / "proof.json").write_text(json.dumps(proof, indent=2) + '\n')
    require(len(workers) == pool + 4 and all(row["allowed"] == mask for row in workers),
            f"{arm} thread count or mask differs from requested placement")
    data = json.loads((output / "result.json").read_text())
    return verify_repetitions(data, arm, pool, cases, min_time)


def verify_repetitions(data, arm, pool, cases, min_time):
    context = data["context"]
    require(int(context["auth_bench_cores"]) == pool and
            context["library_build_type"] == "release", f"{arm}: unexpected pool or build type")
    rows = [row for row in data["benchmarks"] if row["run_type"] == "iteration"]
    require({row["run_name"] for row in rows} == set(cases),
            f"{arm}: unexpected cold case set")
    rates = {}
    for case, accounts in cases.items():
        repetitions = [row for row in rows if row["run_name"] == case]
        require(len(repetitions) == 3 and all(
            row["repetition_index"] == index and row["iterations"] > 0 and
            (min_time != "64x" or row["iterations"] == 64) and
            row["db_lookups_per_iter"] == row["accounts"] == accounts and
            not row.get("error_occurred") and not row.get("label") and
            row["txs_per_s"] > 0
            for index, row in enumerate(repetitions)),
            f"{arm}: unexpected repetitions, lookups, or label for {case}")
        rates[case] = [row["txs_per_s"] for row in repetitions]
    return rates


def main(binary, collection, dump_dir, min_time):
    require(min_time in {"1s", "64x"}, "unexpected benchmark minimum time")
    adaptive = min_time == "1s"
    cases = ADAPTIVE_CASES if adaptive else {CASE: 64}
    order = ADAPTIVE_ORDER if adaptive else ORDER
    binary = binary.resolve(strict=True)
    output = dump_dir / ("auth-cold-adaptive" if adaptive else "auth-cold64-capacity")
    output.mkdir(parents=True)
    with binary.open("rb") as stream:
        sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    metadata = dict(source=os.getenv("GIT_COMMIT"), binary_sha256=sha256,
                    host=os.getenv("HOG_HOST"), topology=topology())
    (output / "provenance.json").write_text(json.dumps(metadata, indent=2) + '\n')
    runs = []
    for index, arm in enumerate(order, 1):
        directory = output / f"{index}-{arm}"
        directory.mkdir()
        rates = run_arm(binary, directory, arm, cases, min_time)
        runs.append(dict(arm=arm, rep_tx_per_s=rates if adaptive else rates[CASE]))
    if adaptive:
        report = {case: {arm: median(rate for row in runs if row["arm"] == arm
                                     for rate in row["rep_tx_per_s"][case])
                         for arm in dict.fromkeys(order)} for case in cases}
    else:
        report = {arm: median(rate for row in runs if row["arm"] == arm
                              for rate in row["rep_tx_per_s"]) for arm in dict.fromkeys(order)}
    (output / "summary.json").write_text(json.dumps(dict(runs=runs, median_tx_per_s=report),
                                                     indent=2) + '\n')
    shutil.copyfile(output / "1-stock/result.json", collection / "auth_bench.json")


if __name__ == "__main__":
    try:
        binary, collection, dump_dir, min_time = sys.argv[1:]
        main(Path(binary), Path(collection), Path(dump_dir), min_time)
    except (KeyError, OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"Cold Auth capacity diagnostic: {error}")
