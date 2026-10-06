#!/usr/bin/env python3
"""Run the native submission benchmark against an owned, fresh hog-devnode."""

import argparse
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time


def interrupt(signum, frame):
    # Unwind subprocess.run and the node's finally block on CI cancellation.
    raise KeyboardInterrupt


def corpus_timeout(args):
    # The client signs its whole corpus before the measured window opens, so
    # the allowance has to scale with the corpus and not just the window. A
    # millisecond per transaction is roughly twice what the bench hosts need.
    return args.seconds + args.warmup + 180 + args.transactions // 1000


def main():
    signal.signal(signal.SIGTERM, interrupt)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path,
                        help="new directory for results and logs")
    parser.add_argument("--connections", type=int, default=4)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--transactions", type=int, default=60000)
    parser.add_argument("--block-ms", type=int, default=500)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--accounts", type=int, default=100)
    args = parser.parse_args()
    for key, limit in (("connections", 64), ("seconds", 60), ("warmup", 10),
                       ("transactions", 1000000), ("block_ms", 10000),
                       ("workers", 256), ("accounts", 1000)):
        if not 1 <= getattr(args, key) <= limit:
            parser.error(f"{key} must be between 1 and {limit}")
    build = args.build.resolve()
    node_bin = build / "hogd/devnode/hog-devnode"
    bench_bin = build / "hogd/hog_mempool/test/schedgateway_client_bench"
    for binary in (node_bin, bench_bin):
        if not os.access(binary, os.X_OK):
            parser.error(f"build {binary} first")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env["DEFAULT_THREADPOOL_SIZE"] = str(args.workers)
    # The program-specific override takes precedence over the general setting.
    env["DEFAULT_THREADPOOL_SIZE_hog-devnode"] = str(args.workers)
    env["DEFAULT_THREADPOOL_SIZE_schedgateway_client_bench"] = "8"
    metadata = {"host": platform.node(), "platform": platform.platform(),
                "cpuCount": os.cpu_count(), "backendWorkers": args.workers,
                "accounts": args.accounts, "ramdbBytes": 2147483648}
    (output / "host.json").write_text(json.dumps(metadata, indent=2) + "\n")
    # hog-devnode is bootstrapped from explicit public allocations and never
    # sees a signer, so the benchmark emits the allocations for the accounts it
    # holds the keys to and re-derives those keys when it runs.
    bootstrap = output / "bootstrap.json"
    subprocess.run([str(bench_bin), "--emit-bootstrap-manifest",
                    str(bootstrap), str(args.accounts)], env=env, check=True)
    with tempfile.TemporaryDirectory(prefix="schedgateway-bench-") as data, \
            (output / "node.log").open("w") as node_log, \
            (output / "readiness.json").open("w") as readiness, \
            (output / "client.log").open("w") as client_log:
        node = subprocess.Popen([
            str(node_bin), "--data-dir", data, "--listen-host", "127.0.0.1",
            "--scheduler-port", "0", "--bootstrap-manifest", str(bootstrap),
            "--ramdb-bytes", "2147483648", "--log-level", "error",
        ], env=env, stdout=readiness, stderr=node_log)
        try:
            deadline = time.monotonic() + 60
            while not (output / "readiness.json").read_text().endswith("\n"):
                if node.poll() is not None:
                    raise RuntimeError(
                        f"hog-devnode exited; see {output / 'node.log'}")
                if time.monotonic() >= deadline:
                    raise TimeoutError("hog-devnode startup timed out")
                time.sleep(0.05)
            ready = json.loads((output / "readiness.json").read_text())
            manifest = Path(ready["manifestPath"])
            (output / "manifest.json").write_bytes(manifest.read_bytes())
            subprocess.run([
                str(bench_bin), str(manifest), str(output / "result.json"),
                str(args.connections), str(args.seconds), str(args.warmup),
                str(args.transactions), str(args.block_ms),
            ], env=env, stdout=client_log, stderr=client_log, check=True,
                timeout=corpus_timeout(args))
            result = json.loads((output / "result.json").read_text())
            admitted = sum(result[k] for k in
                           ("measuredSuccess", "warmupSuccess",
                            "drainSuccess"))
            if result["error"] or not result["measuredSuccess"] or \
                    result["committedTransactions"] != admitted:
                raise RuntimeError(
                    "benchmark did not reconcile admitted transactions")
            print(f"{result['measuredSuccess'] / result['seconds']:.1f} tx/s; "
                  f"p50 {result['latencyP50Us']} us; "
                  f"p99 {result['latencyP99Us']} us")
            print(output / "result.json")
        finally:
            if node.poll() is None:
                node.terminate()
                try:
                    node.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    node.kill()
                    node.wait()


if __name__ == "__main__":
    main()
