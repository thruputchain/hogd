#!/usr/bin/env python3
"""Reduce the stopped coordinator's stock INFO log to typed event receipts."""

from collections import Counter, defaultdict
from datetime import datetime, timezone
from ipaddress import IPv4Address
import json
import os
from pathlib import Path
import re
import stat
import sys

MAX_BYTES = 512 << 20
MAX_LINE = 64 << 10
MAX_EVENTS = 4096
TMP_LOG = re.compile(r"/tmp/hogd_stderr_[0-9]{1,24}_[0-9]{1,12}_[a-zA-Z0-9]{6}")
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


PROPOSAL_REPLY_MARKER = "schedulercoord_propose_block;"
PROPOSAL_REPLY = re.compile(
    r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,9})?(?:Z|[+-]\d\d:\d\d) "
    r"\?\?\? \d{1,12}\.\d{9}, CLIENT, REPLY, ([1-9][0-9]{0,9}), "
    r"([0-9.]{7,15}:[1-9][0-9]{0,4}), ([0-9.]{7,15}:[1-9][0-9]{0,4}), "
    r"schedulercoord_propose_block; io=([^ ,]+), queue=([^ ,]+), serde=([^ ,]+)\n"
)
DURATION_PART = re.compile(r"([0-9]{1,12})(?:\.([0-9]{1,9}))?(ns|µs|ms|s|m|h)")
NANOSECONDS = {"ns": 1, "µs": 1000, "ms": 10**6, "s": 10**9, "m": 60 * 10**9, "h": 3600 * 10**9}


def go_duration_ns(value):
    total = 0
    start = 0
    largest = 10**15
    for part in DURATION_PART.finditer(value):
        whole, fraction, unit = part.groups()
        ns = NANOSECONDS[unit]
        if part.start() != start or ns >= largest or (fraction and part.end() != len(value)):
            raise ValueError("invalid Go RPC duration")
        total += int(whole) * ns
        if fraction:
            fractional, remainder = divmod(int(fraction) * ns, 10 ** len(fraction))
            if remainder:
                raise ValueError("invalid fractional nanoseconds")
            total += fractional
        start = part.end()
        largest = ns
    if start == 0 or start != len(value) or total > 240 * 10**9:
        raise ValueError("Go RPC duration is outside bounds")
    return total


def proposal_reply_stats(line):
    match = PROPOSAL_REPLY.fullmatch(line)
    if not match:
        raise ValueError("malformed or truncated Go proposal reply stats")
    xid, server, client, receive, queue, decode = match.groups()
    server_ip, server_port = server.rsplit(":", 1)
    client_ip, client_port = client.rsplit(":", 1)
    if not (int(xid) < 2**32 and 0 < int(server_port) < 2**16 and 0 < int(client_port) < 2**16):
        raise ValueError("Go proposal reply identity is outside bounds")
    IPv4Address(server_ip)
    IPv4Address(client_ip)
    return {
        "xid": int(xid),
        "server_port": int(server_port),
        "client_port": int(client_port),
        "record_receive_ns": go_duration_ns(receive),
        "receive_queue_ns": go_duration_ns(queue),
        "decode_ns": go_duration_ns(decode),
    }



PROPOSAL_TIMING = re.compile(
    r"CHUCKD_PROPOSE_TIMING height=([1-9][0-9]{0,14}) block_id=([1-9][0-9]{0,19}) "
    r"tx_count=1000000 value=([a-f0-9]{12}) rpc_ns=([1-9][0-9]{0,11}) "
    r"local_ns=([1-9][0-9]{0,11}) reply_end_ns=([1-9][0-9]{0,12})"
)
COMMIT_REQUEST_TIMING = re.compile(
    r"CHUCKD_COMMIT_REQUEST_TIMING height=([1-9][0-9]{0,14}) block_id=([1-9][0-9]{0,19}) "
    r"value=([a-f0-9]{12}) request_ns=([1-9][0-9]{0,12})"
)


def keyed_timing(body):
    if match := PROPOSAL_TIMING.fullmatch(body):
        height, block, value, rpc, local, reply_end = match.groups()
        row = {"rpc_ns": int(rpc), "local_ns": int(local), "reply_end_ns": int(reply_end)}
        kind = "proposal"
    elif match := COMMIT_REQUEST_TIMING.fullmatch(body):
        height, block, value, request = match.groups()
        row = {"request_ns": int(request)}
        kind = "commit_request"
    else:
        raise ValueError("malformed historical proposal timing")
    if not 0 < int(block) < 2**64:
        raise ValueError("invalid historical proposal timing block id")
    return kind, (int(height), int(block), value), row | {
        "height": int(height), "block_id": int(block), "value": value}


def unique_row(records, key, row, cap):
    if key in records or len(records) >= cap:
        raise ValueError("duplicate or excessive historical timing records")
    records[key] = row


def reduce_log(stream, size, single=False, post_proposal=False):
    events, omitted = [], Counter()
    consumed = 0
    origin = None
    truncated = False
    timings = {"proposal": {}, "commit_request": {}, "reply_stats": {}}
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
        if post_proposal and b", CLIENT, REPLY, " in raw and PROPOSAL_REPLY_MARKER.encode() in raw:
            row = proposal_reply_stats(ANSI.sub(b"", raw).decode("utf-8"))
            key = row["xid"], row["client_port"], row["server_port"]
            unique_row(timings["reply_stats"], key, row, 2048)
            continue
        if b"/sub/baton/baton/" not in raw and (not single or b"/sub/catchup/cbpool.go:" not in raw):
            continue
        line = ANSI.sub(b"", raw).decode("ascii", errors="replace").rstrip("\r\n")
        match = HEADER.fullmatch(line)
        if not match:
            continue
        body = match["body"]
        if post_proposal and body.startswith(("CHUCKD_PROPOSE_TIMING", "CHUCKD_COMMIT_REQUEST_TIMING")):
            kind, key, row = keyed_timing(body)
            if match["file"] != {"proposal": "propose", "commit_request": "commit"}[kind]:
                raise ValueError("historical timing emitted by unexpected source file")
            unique_row(timings[kind], key, row, 256)
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
                "NOMINAT", "COMMITTED", "waiting for pre-persisting", "waiting for persisting",
                "finished waiting for catchup to pre-persist", "catchup need not wait for pre-persist",
            )):
                omitted["unrecognized_selected_lines"] += 1
    result = {"source_bytes": size, "prior_tail_truncation": truncated, "omitted": dict(omitted),
            "clock": "coordinator-local UTC log", "origin_unix_ns": origin, "events": events,
            **validate_joins(events)}
    if post_proposal:
        result["post_proposal"] = {key: list(records.values()) for key, records in timings.items()}
        result["post_proposal"]["clock"] = "Go process-local monotonic elapsed; reply stats are separate durations."
    return result


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


def summarize_single(events):
    phases = defaultdict(list)
    for row in events:
        if "value" in row:
            phases[row["value"]].append(row)
    joins = []
    for value, rows in phases.items():
        kinds = defaultdict(list)
        for row in rows:
            kinds[row["kind"]].append(row)
        commits = [row for row in kinds["committed"] if row["tx_count"] == 1_000_000]
        if not commits:
            continue
        heights = {r["height"] for r in rows if "height" in r}
        result = {"value": value, "heights": sorted(heights), "status": "incomplete"}
        stages = ("nomination_start", "nomination_end", "externalized", "committed",
                  "prepersist_start", "persist_start")
        if len(heights) != 1 or any(len(kinds[x]) > 1 for x in stages):
            result["status"] = "ambiguous"
        elif all(len(kinds[x]) == 1 for x in stages):
            offsets = {x: kinds[x][0]["offset_ns"] for x in stages}
            # NOMINATED follows DisseminateBasis in the proposal goroutine,
            # independently of the externalization callback and commit.
            result["local_offsets_ns"] = offsets
            result["nomination_callback_order"] = {
                phase: ("before" if offsets["nomination_end"] < offsets[phase]
                        else "after" if offsets["nomination_end"] > offsets[phase]
                        else "same_timestamp")
                for phase in ("externalized", "persist_start", "committed")
            }
            ordered = ("nomination_start", "prepersist_start",
                       "externalized", "persist_start", "committed")
            steps = [offsets[phase] for phase in ordered]
            if steps != sorted(steps) or offsets["nomination_end"] < offsets["prepersist_start"]:
                result["status"] = "out_of_order_or_overlapping"
            elif any(kinds[phase][0]["tx_count"] != 1_000_000
                     for phase in ("nomination_start", "nomination_end")):
                result["status"] = "ambiguous"
            else:
                finishes = kinds["prepersist_wait_done"] + kinds["prepersist_no_wait"]
                start = offsets["prepersist_start"]
                end = min(offsets["nomination_end"], offsets["externalized"])
                hits = [row for row in finishes if start <= row["offset_ns"] <= end]
                if len(hits) == 1:
                    result.update(status="joined_observed_info",
                                  prepersist_wait_outcome=hits[0]["kind"],
                                  prepersist_wait_end_ns=hits[0]["offset_ns"])
                elif hits:
                    result["status"] = "ambiguous"
        joins.append(result)
    return joins


EVENT_COMMIT = re.compile(
    r"^I\[[0-9]{4} [0-9:]{8} rpc_context\.cc:[0-9]{1,4}\] "
    r"block committed height=([1-9][0-9]{0,18}) block_id=([1-9][0-9]{0,19}) "
    r"((?:[a-z_]+=[0-9]+(?:\.[0-9]{6})? )*)outcome=ok\n$"
)
EVENT_SLOW = re.compile(
    r"^W\[[0-9]{4} [0-9:]{8} event_block_context\.cc:[0-9]{1,4}\] "
    r"slow block phase phase=([a-z_]{1,48}) block_id=([1-9][0-9]{0,19}) "
    r"elapsed_ns=([0-9]{1,19})\n$"
)
EVENT_FIELDS = (
    "sync_ns", "pending_writes_ns", "publish_ns", "close_total_ns",
    "wait_executors_closed_s", "wait_lock_s", "prepare_context_and_async_s",
)


def reduce_event_log(stream, size):
    consumed = 0
    commits = {}
    slow = []
    omitted = Counter()
    while consumed < size:
        raw = stream.readline(min(MAX_LINE + 1, size - consumed))
        if not raw:
            raise ValueError("Event log shrank during capture")
        consumed += len(raw)
        if len(raw) > MAX_LINE:
            omitted["oversize_lines"] += 1
            while not raw.endswith(b"\n") and consumed < size:
                raw = stream.readline(min(MAX_LINE + 1, size - consumed))
                if not raw:
                    raise ValueError("Event log shrank during capture")
                consumed += len(raw)
            continue
        if not raw.endswith(b"\n"):
            omitted["partial_final_line"] += 1
        if raw.startswith(b"[truncated by ci_truncate_log_to_tail:"):
            omitted["prior_tail_truncation"] += 1
        if b"block committed height=" not in raw and b"slow block phase phase=" not in raw:
            continue
        line = ANSI.sub(b"", raw).decode("ascii", errors="replace")
        if match := EVENT_COMMIT.fullmatch(line):
            height, block, text = match.groups()
            words = text.split()
            fields = dict(word.split("=", 1) for word in words)
            if len(fields) != len(words) or int(block) >= 2**64:
                raise ValueError("ambiguous Event commit record")
            row = {"height": int(height), "block_id": int(block)}
            for field in EVENT_FIELDS:
                if field not in fields:
                    omitted[f"missing_{field}"] += 1
                elif field.endswith("_ns"):
                    row[field] = int(fields[field])
                else:
                    row[field] = float(fields[field])
            unique_row(commits, (row["height"], row["block_id"]), row, 4096)
        elif match := EVENT_SLOW.fullmatch(line):
            phase, block, elapsed = match.groups()
            if int(block) >= 2**64 or int(elapsed) <= 1_000_000_000 or len(slow) >= 4096:
                raise ValueError("invalid Event slow-phase receipt")
            slow.append({"phase": phase, "block_id": int(block), "elapsed_ns": int(elapsed)})
        else:
            raise ValueError("malformed Event timing record")
    return {"source_bytes": size, "commit_records": list(commits.values()), "slow_warnings": slow,
            "omitted": dict(omitted),
            "scope": "Stock Event commit summaries and >1s warnings. Recovery without a live context can omit timings."}


def capture(path, single=False, post_proposal=False, event=False):
    if not TMP_LOG.fullmatch(path) or os.path.realpath(path) != path:
        raise ValueError("unexpected control stderr path")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid()
                or before.st_nlink != 1 or not 0 < before.st_size <= MAX_BYTES):
            raise ValueError("control stderr must be a bounded owned regular file")
        result = (reduce_event_log(stream, before.st_size) if event
                  else reduce_log(stream, before.st_size, single, post_proposal))
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("control stderr changed during capture")
    return result


def main():
    args = sys.argv[1:]
    if args[:1] == ["event"]:
        _, path = args
        host, run, sha = (os.environ[key] for key in ("HOG_HOST", "HOG_XID", "GIT_COMMIT"))
        if (host != "hog9" or os.environ.get("HOGD_CI_CONTAINER") != "1"
                or os.environ.get("HOGD_CI_OFFLINE") != "1"
                or os.environ.get("GIT_BRANCH") != "codex/combined-native-single-post-proposal"
                or not re.fullmatch(r"hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*", run)
                or not re.fullmatch(r"[a-f0-9]{40}", sha)):
            raise ValueError("unexpected Event diagnostic role or mode")
        receipt = capture(path, event=True)
        clean = receipt["commit_records"] and not any(receipt["omitted"].get(key)
            for key in ("oversize_lines", "partial_final_line", "prior_tail_truncation"))
        receipt.update(schema=1, host=host, run=run, source_sha=sha,
                       status="observed" if clean else "incomplete")
        write_receipt(Path("/workspace/.ci/dumps/native-single-event.json"), receipt)
        return clean
    single = args[:1] == ["single"]
    if single:
        args = args[1:]
    node, path = args
    expected = {"0": "hog4", "1": "hog8"}
    host = os.environ["HOG_HOST"]
    run = os.environ["HOG_XID"]
    sha = os.environ["GIT_COMMIT"]
    post_proposal = os.environ.get("GIT_BRANCH") == "codex/combined-native-single-post-proposal"
    if ((single and node != "0") or expected.get(node) != host or os.environ.get("HOGD_CI_CONTAINER") != "1"
            or os.environ.get("HOGD_CI_OFFLINE") != "1"
            or os.environ.get("GIT_BRANCH") not in (
                "codex/combined-native-single-replay", "codex/combined-native-single-proposal-dag",
                "codex/combined-native-single-post-proposal")
            or not re.fullmatch(r"hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*", run)
            or not re.fullmatch(r"[a-f0-9]{40}", sha)):
        raise ValueError("unexpected Combined coordinator role or mode")
    receipt = capture(path, single, post_proposal)
    joins = summarize_single(receipt["events"]) if single else []
    status = "observed" if ((not single or any(r["status"] == "joined_observed_info" for r in joins))
                           and not receipt["prior_tail_truncation"] and receipt["full_committed_values"]
                           and not receipt["omitted"].get("unrecognized_selected_lines")
                           and not receipt["omitted"].get("partial_final_line")) else "incomplete"
    if post_proposal and (not single or not all(
            receipt["post_proposal"][key] for key in ("proposal", "commit_request", "reply_stats"))):
        status = "incomplete"
    receipt.update(schema=1, status=status, node=int(node), host=host, run=run, source_sha=sha)
    if single:
        receipt.update(single_info_joins=joins, interval_scope=(
            "One coordinator's ordered INFO boundaries; no RPC-return timestamp, "
            "disk-completion timestamp, or cross-process timing inferred."))
    if not single:
        raise ValueError("expected single-node coordinator input")
    dest = Path("/workspace/.ci/dumps/native-single-coordinator.json")
    write_receipt(dest, receipt)
    return status == "observed"


def write_receipt(dest, receipt):
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



if __name__ == "__main__":
    try:
        sys.exit(0 if main() else 1)
    except (OSError, ValueError, KeyError) as error:
        detail = str(error) if isinstance(error, ValueError) else error.__class__.__name__
        print(f"Combined coordinator summary: {detail}", file=sys.stderr)
        sys.exit(1)
