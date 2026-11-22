#!/usr/bin/env python3
"""Reduce one coordinator stream without exporting wall timestamps or raw logs."""

from calendar import timegm
from datetime import datetime
import json
import re
import sys

SAMPLE_LIMIT = 50_000
LOG_LIMIT = 64 * 1024 * 1024
ANSI = re.compile(r"\x1b\[[0-9;]*m")
STAMP = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(\d{1,9}))?Z INF ")
HEIGHT = r"(?P<height>\d+)"
VALUE = r"(?P<value>[a-f0-9]{12})\b"
COUNT = r"(?P<txs>\d+)"
TRACE = re.compile(r"\btrace=([a-zA-Z0-9_-]{1,32})\b")
EVENTS = {
    "nomination_start": rf"NOMINATING: height {HEIGHT}; tx_count {COUNT}; value {VALUE}",
    "nomination_done": rf"NOMINATED: height {HEIGHT}; tx_count {COUNT}; value {VALUE}",
    "prepersist_wait": rf"waiting for pre-persisting of catchup block: height {HEIGHT}; value {VALUE}",
    "prepersist_done": rf"finished waiting for catchup to pre-persist: value {VALUE}",
    "prepersist_no_wait": rf"catchup need not wait for pre-persist: value {VALUE}",
    "externalized": rf"received externalization for slot {HEIGHT} with value hash {VALUE}",
    "validation_start": rf"VALIDATING: height {HEIGHT}; tx_count {COUNT}; value {VALUE}",
    "validation_done": rf"VALIDATED: height {HEIGHT}; tx_count {COUNT}; value {VALUE}",
    "validation_rpc": rf"scheduler validating: height={HEIGHT}; value={VALUE}",
    "validation_missing_txs": rf"scheduler is missing transactions: height={HEIGHT}; value={VALUE}",
    "validation_too_late": rf"scheduler is too late to validate: height={HEIGHT}; value={VALUE}",
    "validation_invalid": rf"scheduler invalidated: height={HEIGHT}; value={VALUE}",
    "commit_start": rf"committing: height {HEIGHT}; value {VALUE}",
    "persist_wait": rf"waiting for persisting of catchup block: height {HEIGHT}; value {VALUE}",
    "commit_done": rf"COMMITTED: height {HEIGHT}; tx_count {COUNT}; value {VALUE}",
}
EVENTS = {kind: re.compile(pattern) for kind, pattern in EVENTS.items()}
BLOCK = re.compile(
    r"Block duration \(s\), (\d+(?:\.\d+)?), Num txs in block, (\d+), "
    r"Overall duration \(s\), (\d+(?:\.\d+)?), Overall number of txs, (\d+)"
)
THROUGHPUT = re.compile(
    r"Block throughput \(tx/s\), \d+(?:\.\d+)?, "
    r"Overall throughput \(tx/s\), \d+(?:\.\d+)?"
)


def bounded_lines(lines):
    received = 0
    for line in lines:
        received += len(line)
        if received > LOG_LIMIT:
            raise ValueError("coordinator stream exceeds capture limit")
        yield line


def event_rows(lines):
    rows = []
    origin = None
    for raw in bounded_lines(lines):
        line = ANSI.sub("", raw)
        stamp = STAMP.match(line)
        if not stamp:
            continue
        for kind, pattern in EVENTS.items():
            if match := pattern.search(line):
                whole, fraction = stamp.groups()
                seconds = timegm(datetime.strptime(whole, "%Y-%m-%dT%H:%M:%S").timetuple())
                now = seconds * 10**9 + int((fraction or "").ljust(9, "0"))
                if origin is None:
                    origin = now
                fields = {
                    key: (value if key == "value" else int(value))
                    for key, value in match.groupdict().items()
                }
                if trace := TRACE.search(line):
                    fields["trace"] = trace[1]
                rows.append({"phase": kind, "offset_ns": now - origin, **fields})
                if len(rows) > SAMPLE_LIMIT:
                    raise ValueError("too many coordinator events")
                break
    if not rows:
        raise ValueError("no recognizable coordinator timings")
    return {"clock": "one coordinator's UTC log, relative to its first captured event", "events": rows}


def benchmark_rows(lines):
    rows = []
    pending = None
    for raw in bounded_lines(lines):
        line = raw.strip()
        if match := BLOCK.fullmatch(line):
            if pending is not None:
                raise ValueError("missing coordinator throughput line")
            seconds, txs, elapsed, total = match.groups()
            pending = {
                "seconds": float(seconds), "txs": int(txs),
                "overall_seconds": float(elapsed), "overall_txs": int(total),
            }
        elif THROUGHPUT.fullmatch(line) and pending is not None:
            rows.append(pending)
            pending = None
        elif line:
            raise ValueError("unexpected coordinator stdout")
        if len(rows) > SAMPLE_LIMIT:
            raise ValueError("too many coordinator benchmark samples")
    if not rows:
        raise ValueError("no completed coordinator benchmark pairs")
    return {"samples": rows, "incomplete_tail": pending is not None}


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["stdout"]:
            report = benchmark_rows(sys.stdin)
        elif sys.argv[1:] == ["stderr"]:
            report = event_rows(sys.stdin)
        else:
            raise ValueError("expected stdout or stderr")
        print(json.dumps(report))
    except ValueError as error:
        sys.exit(f"Two-cohort coordinator: {error}")
