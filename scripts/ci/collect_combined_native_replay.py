"""Copy only this run's named Payment/Fibonacci metric files to the trusted collector."""

from contextlib import contextmanager
import hashlib
import json
import os
import re
import stat
import sys

BRANCH = "codex/combined-native-rpc-replay"
NATIVE_SOURCE = "71d6a37ebe4918de68950961fadcec72cadb1c2e"
MAX_BYTES = 256 * 1024
METRICS = {
    "fibonacci-rv32only": "hog10",
    "payments-bench-rv32-10": "hog10",
    "payments-bench-rv32-2": "hog10",
}
DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


@contextmanager
def directory_at(path, parent=None):
    descriptor = os.open(".", DIRECTORY, dir_fd=parent)
    try:
        for component in path.split("/"):
            child = os.open(component, DIRECTORY, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        yield descriptor
    finally:
        os.close(descriptor)


def read_metric(metric, file_name):
    directory = f"metrics_history/metrics/{metric}/{BRANCH}"
    with directory_at(directory) as parent:
        flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
        with os.fdopen(os.open(file_name, flags, dir_fd=parent), "rb") as stream:
            file = os.fstat(stream.fileno())
            if not stat.S_ISREG(file.st_mode) or file.st_nlink != 1 or not 0 < file.st_size <= MAX_BYTES:
                raise ValueError("expected a bounded regular metric file")
            data = stream.read(MAX_BYTES + 1)
            if len(data) != file.st_size:
                raise ValueError("metric file changed during collection")
            return data


def write_new(directory, name, data):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
    with os.fdopen(os.open(name, flags, mode=0o600, dir_fd=directory), "wb") as stream:
        stream.write(data)


def collect(source, timestamp, run):
    if not (re.fullmatch(r"[a-f0-9]{40}", source)
            and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}", timestamp)
            and re.fullmatch(r"hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*", run)):
        raise ValueError("invalid run provenance")
    reports = {}
    with directory_at(".ci/dumps") as dumps:
        os.mkdir("combined-native-rpc-replay", mode=0o700, dir_fd=dumps)
        with directory_at("combined-native-rpc-replay", dumps) as output:
            for metric, host in METRICS.items():
                report = reports[metric] = {"host": host}
                try:
                    data = read_metric(metric, f"{timestamp}_{source}")
                except (ValueError, OSError) as error:
                    report["status"] = "missing" if isinstance(error, FileNotFoundError) else "rejected"
                    continue
                write_new(output, metric, data)
                report.update(status="captured", bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            complete = all(report["status"] == "captured" for report in reports.values())
            receipt = {
                "branch": BRANCH, "run": run, "source": source, "native_baseline": NATIVE_SOURCE,
                "scope": "Payment and Fibonacci only", "collector_host": "hog10", "complete": complete, "metrics": reports,
            }
            write_new(output, "receipt.json", (json.dumps(receipt, indent=2) + "\n").encode())
    return complete


if __name__ == "__main__":
    try:
        if not collect(*sys.argv[1:]):
            sys.exit("Incomplete Payment/Fibonacci metric collection; see the diagnostic receipt")
    except (OSError, ValueError) as error:
        sys.exit(f"Payment/Fibonacci metric collection failed: {error}")
