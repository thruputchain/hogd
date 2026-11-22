#!/usr/bin/env python3
"""Collect only Event close counters and normal coordinator benchmark pairs."""

import json
import re
import sys

from nightly_single_stage import COORDINATOR_BLOCK, COORDINATOR_THROUGHPUT

COMMITTED = re.compile(
    r"\bblock committed height=(\d+) block_id=(\d+) (.*?)outcome=ok\s*$"
)
FIELDS = {
    "open_lock_wait_ns", "close_lock_wait_ns", "commit_lock_wait_ns",
    "write_events_ns", "pending_writes_ns", "residual_write_ns",
    "residual_buffers", "sync_ns", "publish_ns", "wait_executors_closed_s",
    "wait_lock_s", "prepare_context_and_async_s", "close_total_s",
    "close_total_ns", "residual_bytes",
}


def event_rows(lines):
    rows = {}
    for line in lines:
        match = COMMITTED.search(line)
        if not match:
            continue
        height, block_id = map(int, match.groups()[:2])
        raw = dict(re.findall(r"\b(\w+)=(\d+(?:\.\d+)?)(?=\s|$)", match[3]))
        fields = {
            key: (float(value) if key.endswith("_s") else int(value))
            for key, value in raw.items() if key in FIELDS
        }
        if (height, block_id) in rows:
            raise ValueError("duplicate Event commit identity")
        rows[height, block_id] = {"height": height, "block_id": block_id, **fields}
    if not any("close_total_ns" in row for row in rows.values()):
        raise ValueError("no Event close phases found")
    return [rows[key] for key in sorted(rows)]


def coordinator_pairs(lines):
    pending = None
    output = []
    for raw in lines:
        line = raw.strip()
        if COORDINATOR_BLOCK.fullmatch(line) and pending is None:
            pending = line
        elif COORDINATOR_THROUGHPUT.fullmatch(line) and pending is not None:
            output.extend((pending, line))
            pending = None
        elif line:
            raise ValueError("unexpected coordinator stdout")
    if not output:
        raise ValueError("no complete coordinator benchmark pair")
    return output


if __name__ == "__main__":
    if sys.argv[1:] == ["event"]:
        json.dump(event_rows(sys.stdin), sys.stdout, indent=2)
        print()
    elif sys.argv[1:] == ["coordinator"]:
        print("\n".join(coordinator_pairs(sys.stdin)))
    else:
        sys.exit("expected event or coordinator")
