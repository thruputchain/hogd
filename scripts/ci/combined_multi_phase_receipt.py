#!/usr/bin/env python3
"""Reduce the stopped coordinator's stock INFO log to typed event receipts."""

from collections import Counter, defaultdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import stat
import sys

MAX_BYTES = 512 << 20
MAX_LINE = 64 << 10
MAX_EVENTS = 4096
ANSI = re.compile(rb"\x1b\[[0-9;]*m")
HEADER = re.compile(
    r"^(?P<time>\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(?P<ns>\d{1,9}))?Z INF "
    r"/workspace/thruput-go/chuckd/sub/(?P<directory>baton/baton/|catchup/)(?P<file>vproposal|propose|commit|scp|cbpool)\.go:"
    r"\d{1,4} (?P<body>[^\r\n]*)$"
)
HEIGHT = r"(?P<height>[0-9]{1,15})"
VALUE = r"(?P<value>[a-f0-9]{12})"
TXS = r"(?P<tx_count>[0-9]{1,9})"
TRACE = r" trace=(?P<trace>[a-z]{1,16})"
TYPED = rf"height {HEIGHT}; tx_count {TXS}; value {VALUE}"
CANDIDATE = rf"height={HEIGHT}; value={VALUE}"
PROPOSER = r"; proposer vid:[a-f0-9]{6}"
PATTERNS = {
    "vproposal": (
        ("validation_start", rf"VALIDATING: {TYPED}{PROPOSER}{TRACE}"),
        ("validation_end", rf"VALIDATED: {TYPED}{PROPOSER}{TRACE}"),
        ("validation_rpc", rf"scheduler validating: {CANDIDATE}{TRACE}"),
        ("missing_transactions", rf"scheduler is missing transactions: {CANDIDATE}{TRACE}"),
        ("invalid", rf"scheduler invalidated: {CANDIDATE}{TRACE}"),
        ("too_late", rf"scheduler is too late to validate: {CANDIDATE}{TRACE}"),
        ("missing_parent", rf"scheduler cannot validate due to missing parent block: {CANDIDATE}{TRACE}"),
        ("rpc_error", rf"cannot validate, scheduler error \((?P<error>[^\r\n]{{1,2048}})\){TRACE}"),
    ),
    "propose": (
        ("nomination_start", rf"NOMINATING: {TYPED}; scp_value: [a-f0-9]{{6}}"),
        ("nomination_end", rf"NOMINATED: {TYPED}"),
    ),
    "commit": (
        ("committed", rf"COMMITTED: {TYPED}{PROPOSER}"),
        ("commit_start", rf"committing: height {HEIGHT}; value {VALUE}; proposer vid:[a-f0-9]{{6}}"),
    ),
    "cbpool": (
        ("prepersist_start", rf"waiting for pre-persisting of catchup block: height {HEIGHT}; value {VALUE}"),
        ("persist_start", rf"waiting for persisting of catchup block: height {HEIGHT}; value {VALUE}"),
        ("prepersist_wait_done", rf"finished waiting for catchup to pre-persist: value {VALUE}"),
        ("prepersist_no_wait", rf"catchup need not wait for pre-persist: value {VALUE}"),
    ),
    "scp": (
        ("externalized", rf"received externalization for slot {HEIGHT} with value hash {VALUE}{TRACE}"),
    ),
}
PATTERNS = {file: tuple((kind, re.compile(expr)) for kind, expr in patterns)
            for file, patterns in PATTERNS.items()}
VALIDATION = {"validation_start", "validation_rpc", "missing_transactions", "validation_end", "invalid",
              "too_late", "missing_parent", "rpc_error"}
FAILURES = {"invalid", "too_late", "missing_parent", "rpc_error"}


def reduce_log(stream, size, single=False):
    events, omitted = [], Counter()
    consumed = 0
    origin = None
    truncated = False
    while consumed < size:
        raw = stream.readline(min(MAX_LINE + 1, size - consumed))
        if not raw:
            raise ValueError("log shrank during capture")
        consumed += len(raw)
        if raw.startswith(b"[truncated by ci_truncate_log_to_tail:"):
            truncated = True
        if len(raw) > MAX_LINE:
            omitted["oversize_lines"] += 1
            while not raw.endswith(b"\n") and consumed < size:
                raw = stream.readline(min(MAX_LINE + 1, size - consumed))
                if not raw:
                    raise ValueError("log shrank during capture")
                consumed += len(raw)
            continue
        if not raw.endswith(b"\n"):
            omitted["partial_final_line"] += 1
            continue
        if b"/sub/baton/baton/" not in raw and (not single or b"/sub/catchup/cbpool.go:" not in raw):
            continue
        line = ANSI.sub(b"", raw).decode("ascii", errors="replace").rstrip("\r\n")
        match = HEADER.fullmatch(line)
        if not match:
            continue
        cbpool = match["file"] == "cbpool"
        if (cbpool and not single) or cbpool != (match["directory"] == "catchup/"):
            continue
        for kind, pattern in PATTERNS[match["file"]]:
            fields = pattern.fullmatch(match["body"])
            if not fields:
                continue
            stamp = datetime.strptime(match["time"], "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
            now = int(stamp.timestamp()) * 10**9 + int((match["ns"] or "").ljust(9, "0"))
            if origin is None:
                origin = now
            row = {"kind": kind, "offset_ns": now - origin}
            for key, value in fields.groupdict().items():
                if key in {"height", "tx_count"}:
                    row[key] = int(value)
                elif key == "error":
                    row[key] = "transport_closed" if value == "transport is closed" else "other"
                else:
                    row[key] = value
            events.append(row)
            if len(events) > MAX_EVENTS:
                raise ValueError("too many coordinator events")
            break
        else:
            if any(match["body"].startswith(p) for p in (
                "VALIDAT", "scheduler validating", "scheduler is missing", "scheduler invalidated",
                "scheduler is too late", "scheduler cannot validate", "cannot validate", "received externalization",
                "NOMINAT", "COMMITTED", "committing:", "waiting for pre-persisting", "waiting for persisting",
                "finished waiting for catchup to pre-persist", "catchup need not wait for pre-persist",
            )):
                omitted["unrecognized_selected_lines"] += 1
    return {"source_bytes": size, "prior_tail_truncation": truncated, "omitted": dict(omitted),
            "clock": "coordinator-local UTC log", "origin_unix_ns": origin, "events": events,
            **validate_joins(events)}


def validate_joins(events):
    keys_by_trace = defaultdict(set)
    for row in events:
        if row["kind"] in VALIDATION and "height" in row:
            keys_by_trace[row["trace"]].add((row["height"], row["value"], row["trace"]))
    by_key = defaultdict(list)
    joins = []
    unjoined_errors = 0
    ambiguous_error_keys = set()
    committed = {(row["height"], row["value"]) for row in events if row["kind"] == "committed"}
    for row in events:
        if row["kind"] not in VALIDATION:
            continue
        if "height" in row:
            key = (row["height"], row["value"], row["trace"])
        elif len(keys_by_trace[row["trace"]]) == 1:
            key, = keys_by_trace[row["trace"]]
        else:
            unjoined_errors += 1
            ambiguous_error_keys.update(keys_by_trace[row["trace"]])
            continue
        by_key[key].append(row)
    for (height, value, trace), rows in sorted(by_key.items()):
        kinds = [row["kind"] for row in rows]
        found = set(kinds)
        body = kinds[1:-1]
        flags = found & FAILURES
        if (height, value, trace) in ambiguous_error_keys or kinds.count("validation_start") > 1 or kinds.count("validation_end") > 1:
            result = "ambiguous"
        elif flags:
            result = "logged_failure" if len(flags) == 1 else "ambiguous"
        elif (len(kinds) >= 3 and kinds[0] == "validation_start" and kinds[-1] == "validation_end"
              and body == ["validation_rpc", "missing_transactions"] * (len(body) // 2) + ["validation_rpc"]
              and all(a["offset_ns"] <= b["offset_ns"] for a, b in zip(rows, rows[1:]))):
            # VALIDATED is emitted by defer. No logged error does not prove an OK reply.
            result = "ended_without_logged_failure"
        else:
            result = "incomplete"
        joins.append({"height": height, "value": value, "trace": trace, "result": result,
                      "rpc_attempts": kinds.count("validation_rpc"),
                      "missing_results": kinds.count("missing_transactions"),
                      "value_committed": (height, value) in committed})
    return {"validation_joins": joins, "unjoined_rpc_errors": unjoined_errors,
            "full_committed_values": sum(row["kind"] == "committed" and row["tx_count"] == 1_000_000
                                         for row in events)}


def capture(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid()
                or before.st_nlink != 1 or not 0 < before.st_size <= MAX_BYTES):
            raise ValueError("control stderr must be a bounded owned regular file")
        # Include typed CBPool persistence markers in this multi-node receipt.
        result = reduce_log(stream, before.st_size, True)
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("control stderr changed during capture")
    return result


def multi_validation_observed(receipt):
    # A validator independently qualifies when it observed a complete
    # million-transaction validation. An immediate success is valid.
    full_starts = {
        (event["height"], event["value"], event["trace"])
        for event in receipt["events"]
        if event["kind"] == "validation_start" and event["tx_count"] == 1_000_000
    }
    return any(
        (join["height"], join["value"], join["trace"]) in full_starts
        and join["result"] == "ended_without_logged_failure"
        for join in receipt["validation_joins"]
    )


def main():
    node, path_arg = sys.argv[1:]
    path = Path(path_arg)
    expected = {"0": "hog4", "1": "hog8"}
    host = os.environ["HOG_HOST"]
    run = os.environ["HOG_XID"]
    sha = os.environ["GIT_COMMIT"]
    branch = "codex/combined-multi-phase-receipt"
    coordinator = expected.get(node)
    if (coordinator is None or host != "hog10" or os.environ.get("HOGD_CI_CONTAINER") != "1"
            or os.environ.get("HOGD_CI_OFFLINE") != "1"
            or os.environ.get("GIT_BRANCH") != branch
            or os.environ.get("MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS") != "1"
            or os.environ.get("HOG_DUMP_DIR") != "/workspace/.ci/dumps"
            or not re.fullmatch(r"hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*", run)
            or not re.fullmatch(r"[a-f0-9]{40}", sha)):
        raise ValueError("unexpected Combined coordinator role or mode")
    directory = Path('/workspace/metrics_history/metrics/nightly_integration_mempool_multi_node') / branch
    if (path.resolve(strict=True) != path or path.parent.parent != directory
            or not path.parent.name.endswith(f"_{sha}_logs")
            or path.name != f"{coordinator}_node{node}_chuckd.stderr.log"):
        raise ValueError("unexpected copied coordinator stderr path")
    receipt = capture(path)
    status = "observed" if (not receipt["prior_tail_truncation"]
                           and receipt["full_committed_values"] and not receipt["omitted"]
                           and multi_validation_observed(receipt)) else "incomplete"
    receipt.update(schema=1, status=status, node=int(node), controller=host,
                   coordinator=coordinator, run=run, source_sha=sha,
                   interval_scope="Coordinator-local INFO timestamps; nested observations may overlap.")
    # The source log remains subject to the ordinary truncation policy. Only
    # compact typed numeric/identity records go to the private artifact.
    dest = Path(f"/workspace/.ci/dumps/multi-coordinator-phases-{coordinator}.json")
    if dest.parent.resolve(strict=True) != dest.parent or dest.parent.stat().st_uid != os.getuid():
        raise ValueError("unexpected coordinator output directory")
    pending = dest.with_suffix(".pending")
    with pending.open("x") as output:
        json.dump(receipt, output, separators=(",", ":"))
        output.write("\n")
    try:
        os.link(pending, dest, follow_symlinks=False)
    finally:
        pending.unlink()
    return status == "observed"


if __name__ == "__main__":
    try:
        sys.exit(0 if main() else 1)
    except (OSError, ValueError, KeyError) as error:
        detail = str(error) if isinstance(error, ValueError) else error.__class__.__name__
        print(f"Combined coordinator summary: {detail}", file=sys.stderr)
        sys.exit(1)
