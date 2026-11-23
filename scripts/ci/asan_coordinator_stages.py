#!/usr/bin/env python3
"""Reduce the stock ASAN hog4 coordinator logs in their own container."""

from calendar import timegm
from collections import Counter, defaultdict
from ipaddress import IPv4Address
import json
from math import ceil
import re
from statistics import median
import sys
from time import strptime

NUMBER = r"([0-9]+\.[0-9]{2})"
COORDINATOR_BLOCK = re.compile(
    rf"Block duration \(s\), {NUMBER}, Num txs in block, ([0-9]+), "
    rf"Overall duration \(s\), {NUMBER}, Overall number of txs, ([0-9]+)"
)
COORDINATOR_THROUGHPUT = re.compile(
    rf"Block throughput \(tx/s\), {NUMBER}, Overall throughput \(tx/s\), {NUMBER}"
)

ANSI = re.compile(r"\x1b\[[0-9;]*m")
STAMP = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(\d{1,9}))?Z INF ")
VALUE = r"([a-f0-9]{12})\b"
EVENTS = {
    "nomination_start": r"NOMINATING: height (\d+); tx_count (\d+); value " + VALUE,
    "nomination_done": r"NOMINATED: height (\d+); tx_count (\d+); value " + VALUE,
    "prepersist_wait": r"waiting for pre-persisting of catchup block: height (\d+); value " + VALUE,
    "externalized": r"received externalization for slot (\d+) with value hash " + VALUE,
    "commit_start": r"committing: height (\d+); value " + VALUE,
    "persist_wait": r"waiting for persisting of catchup block: height (\d+); value " + VALUE,
    "commit_done": r"COMMITTED: height (\d+); tx_count (\d+); value " + VALUE,
}
EVENTS = {key: re.compile(pattern) for key, pattern in EVENTS.items()}
WAITS = {
    "finished": re.compile(r"finished waiting for catchup to pre-persist: value " + VALUE),
    "not_needed": re.compile(r"catchup need not wait for pre-persist: value " + VALUE),
}
TIMING = re.compile(
    r"CHUCKD_PROPOSE_TIMING height=([1-9][0-9]{0,19}) block_id=([0-9]{1,20}) "
    r"tx_count=1000000 value=([a-f0-9]{12}) rpc_ns=([1-9][0-9]{0,11}) "
    r"local_ns=([1-9][0-9]{0,11})(?=\s|$)"
)
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
    return {
        "xid": int(xid),
        "server_ip": str(IPv4Address(server_ip)),
        "server_port": int(server_port),
        "client_ip": str(IPv4Address(client_ip)),
        "client_port": int(client_port),
        "record_receive_ns": go_duration_ns(receive),
        "receive_queue_ns": go_duration_ns(queue),
        "decode_ns": go_duration_ns(decode),
    }


RECEIVE_FIELDS = (
    "xid", "client_port", "server_port", "payload_bytes", "reads",
    "first_capacity", "last_capacity", "growth_after_first", "relocated_after_first",
    "tcp_before", "tcp_after", "rx_bytes_before", "rx_bytes_after",
    "rtt_before_us", "rtt_after_us", "rcv_space_before", "rcv_space_after",
    "rcv_wnd_before_available", "rcv_wnd_after_available", "rcv_wnd_before",
    "rcv_wnd_after", "local_outbound_retrans_before", "local_outbound_retrans_after",
)
RECEIVE_BOOLS = {"tcp_before", "tcp_after", "rcv_wnd_before_available", "rcv_wnd_after_available"}
RECEIVE_MARKER = "CHUCKD_PROPOSAL_RECEIVE"
RECEIVE = re.compile(
    r"(?:^| )" + RECEIVE_MARKER + " "
    + " ".join(field + ("=(true|false)" if field in RECEIVE_BOOLS else "=([0-9]{1,20})")
               for field in RECEIVE_FIELDS)
    + r"\n$"
)


def proposal_receive(line):
    match = RECEIVE.search(line)
    if not match:
        raise ValueError("malformed large scheduler reply receive receipt")
    row = {
        key: (value == "true" if key in RECEIVE_BOOLS else int(value))
        for key, value in zip(RECEIVE_FIELDS, match.groups(), strict=True)
    }
    if not (
        0 < row["xid"] < 2**32
        and 0 < min(row["client_port"], row["server_port"])
        and max(row["client_port"], row["server_port"]) < 2**16
        and 128 * 2**20 <= row["payload_bytes"] <= 10**9
        and 1 <= row["reads"] <= row["payload_bytes"]
        and 0 < row["first_capacity"] <= row["last_capacity"] < 2**31
        and row["payload_bytes"] <= row["last_capacity"]
        and row["growth_after_first"] < row["reads"]
        and row["relocated_after_first"] < 2**32
    ):
        raise ValueError("large scheduler reply receive fields exceed bounds")
    for phase in ("before", "after"):
        valid = row["tcp_" + phase]
        window = row[f"rcv_wnd_{phase}_available"]
        for name in ("rx_bytes_" + phase, "rtt_" + phase + "_us", "rcv_space_" + phase,
                     "rcv_wnd_" + phase, "local_outbound_retrans_" + phase):
            bits = 64 if name.startswith("rx_bytes") else 32
            if row[name] >= 2**bits or (not valid and row[name]):
                raise ValueError("invalid or unavailable receiver TCP_INFO value")
        if (window and not valid) or (not window and row[f"rcv_wnd_{phase}"]):
            raise ValueError("inconsistent receiver window availability")
    return row


MARKERS = (
    "CHUCKD_PROPOSE_TIMING", RECEIVE_MARKER,
    "NOMINATING:", "NOMINATED:", "waiting for pre-persisting of catchup block:",
    "received externalization for slot ", "committing: height ",
    "waiting for persisting of catchup block:", "COMMITTED:",
    "finished waiting for catchup to pre-persist:", "catchup need not wait for pre-persist:",
)
PHASES = {
    "nomination_start_to_prepersist_wait": ("nomination_start", "prepersist_wait"),
    "prepersist_caller_wait": ("prepersist_wait", "prepersist_done"),
    "prepersist_done_to_externalized": ("prepersist_done", "externalized"),
    "nomination_info_to_nominated_info": ("nomination_start", "nomination_done"),
    "nomination_info_to_commit_entry": ("nomination_start", "commit_start"),
    "nomination_info_to_externalized": ("nomination_start", "externalized"),
    "externalized_to_commit_entry": ("externalized", "commit_start"),
    "commit_entry_to_catchup_persist_wait": ("commit_start", "persist_wait"),
    "catchup_persist_wait_to_commit_done": ("persist_wait", "commit_done"),
    "commit_entry_to_commit_done": ("commit_start", "commit_done"),
}


def normalize_stdout(lines):
    duration = None
    output = []
    tally = 0
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if not raw.endswith("\n"):
            raise ValueError("truncated coordinator stdout record")
        if duration is None and (match := COORDINATOR_BLOCK.fullmatch(line)):
            count, cumulative = int(match[2]), int(match[4])
            if count not in (0, 1_000_000) or cumulative != tally + count:
                raise ValueError("invalid coordinator stdout block counts")
            tally = cumulative
            duration = line
        elif duration is not None and COORDINATOR_THROUGHPUT.fullmatch(line):
            output.extend((duration, line))
            duration = None
        else:
            raise ValueError("unexpected coordinator stdout shape")
        if len(output) > 4_000:
            raise ValueError("too many coordinator stdout records")
    if duration is not None or not output:
        raise ValueError("incomplete coordinator stdout")
    return "\n".join(output) + "\n"


def timestamp(match):
    whole, fraction = match.groups()
    return timegm(strptime(whole, "%Y-%m-%dT%H:%M:%S")) * 10**9 + int(
        (fraction or "").ljust(9, "0")
    )


def reduce_stderr(lines):
    blocks = defaultdict(lambda: defaultdict(list))
    completions = defaultdict(list)
    heights = defaultdict(set)
    matched = Counter()
    proposal_timings = {}
    block_ids = set()
    reply_stats = {}
    large_receipts = {}
    for raw in lines:
        if PROPOSAL_REPLY_MARKER in raw and ", CLIENT, REPLY, " in raw:
            receipt = proposal_reply_stats(ANSI.sub("", raw))
            key = receipt["xid"], receipt["client_port"], receipt["server_port"]
            if key in reply_stats or len(reply_stats) >= 1_000:
                raise ValueError("duplicate or excessive Go proposal reply identities")
            reply_stats[key] = receipt
            continue
        if not any(marker in raw for marker in MARKERS):
            continue
        line = ANSI.sub("", raw)
        clock = STAMP.match(line)
        if not clock or not raw.endswith("\n"):
            raise ValueError("malformed or truncated coordinator INFO record")
        if RECEIVE_MARKER in line:
            receipt = proposal_receive(line)
            key = receipt["xid"], receipt["client_port"], receipt["server_port"]
            if key in large_receipts or len(large_receipts) >= 1_000:
                raise ValueError("duplicate or excessive large scheduler reply receives")
            large_receipts[key] = receipt
            continue
        if "CHUCKD_PROPOSE_TIMING" in line:
            timing = TIMING.search(line)
            if timing is None:
                raise ValueError("malformed Chuckd proposal timing")
            height, block_id, value, rpc_ns, local_ns = timing.groups()
            height, block_id, rpc_ns, local_ns = map(int, (height, block_id, rpc_ns, local_ns))
            key = height, value
            if max(height, block_id) >= 2**64 or key in proposal_timings or (
                height, block_id
            ) in block_ids:
                raise ValueError("invalid or duplicate Chuckd proposal timing identity")
            block_ids.add((height, block_id))
            proposal_timings[key] = {
                "block_id": block_id, "rpc_ns": rpc_ns, "local_ns": local_ns,
            }
            if len(proposal_timings) > 1_000:
                raise ValueError("too many Chuckd proposal timings")
            continue
        for kind, pattern in EVENTS.items():
            event = pattern.search(line)
            if not event:
                continue
            fields = event.groups()
            height, value = int(fields[0]), fields[-1]
            n_txs = int(fields[1]) if len(fields) == 3 else None
            blocks[height, value][kind].append((timestamp(clock), n_txs))
            heights[value].add(height)
            matched[kind] += 1
            break
        else:
            for kind, pattern in WAITS.items():
                event = pattern.search(line)
                if event:
                    completions[event[1]].append((timestamp(clock), kind))
                    matched["prepersist_completion_" + kind] += 1
                    break
            else:
                raise ValueError("unrecognized coordinator INFO stage syntax")
        if sum(matched.values()) > 100_000:
            raise ValueError("too many recognized coordinator INFO records")

    full = {key: events for key, events in blocks.items()
            if any(n == 1_000_000 for _, n in events["commit_done"])}
    if not full:
        raise ValueError("no committed one-million-transaction INFO records")

    committed_heights = Counter(height for height, _ in full)
    if any(count != 1 for count in committed_heights.values()):
        raise ValueError("ambiguous committed values at one height")
    if any(len(events["commit_done"]) != 1 for events in full.values()):
        raise ValueError("ambiguous full commit records")

    distributions = defaultdict(list)
    waiting = Counter()
    coverage = Counter()
    blocks = []
    joined_timings = set()
    for (height, value), events in sorted(full.items()):
        times = {
            kind: items[0][0] for kind, items in events.items()
            if len(items) == 1 and (items[0][1] is None or items[0][1] == 1_000_000)
        }
        coverage.update(times.keys())
        row = {"height": height, "value_prefix": value, "transactions": 1_000_000,
               "present_unique_info": sorted(times), "phase_ms": {}}
        blocks.append(row)
        key = height, value
        if key in proposal_timings and "nomination_start" in times and heights[value] == {height}:
            row["proposal_timing"] = proposal_timings[key]
            joined_timings.add(key)
        # Completion logs carry only the displayed hash. Infer a height only
        # when unique and before any next persistence or externalization event.
        if "prepersist_wait" in times and heights[value] == {height}:
            start = times["prepersist_wait"]
            limits = [times[kind] for kind in ("externalized", "persist_wait",
                       "commit_start", "commit_done") if kind in times]
            candidates = [(end, kind) for end, kind in completions[value]
                          if start <= end < min(limits, default=start)]
            if len(candidates) == 1:
                times["prepersist_done"], outcome = candidates[0]
                waiting[outcome] += 1
                row["prepersist_completion"] = outcome
        for phase, (start, end) in PHASES.items():
            if start in times and end in times and times[end] >= times[start]:
                duration = (times[end] - times[start]) / 1e6
                distributions[phase].append(duration)
                row["phase_ms"][phase] = duration

    def report(values):
        result = {"samples": len(values), "unobserved": len(full) - len(values)}
        if values:
            values.sort()
            result.update(p50_ms=median(values), p90_ms=values[ceil(.9 * len(values)) - 1])
        return result

    result = {
        "full_committed_blocks": len(full),
        "matched_info_records": matched,
        "full_blocks_with_unique_markers": coverage,
        "full_blocks": blocks,
        "prepersist_caller_outcomes": {
            "finished": waiting["finished"],
            "not_needed": waiting["not_needed"],
            "unobserved": len(full) - sum(waiting.values()),
        },
        "nominal_phases": {name: report(distributions[name]) for name in PHASES},
        "clock": "Single hog4 process nominal UTC log timestamps; no monotonic clock exposed.",
        "limitations": "NOMINATING follows mempool response decode and local hashing. "
                       "INFO exposes caller waiting, not actual storage boundaries. "
                       "Catchup-persist wait to commit-done includes the scheduler commit RPC. "
                       "No cross-host timestamp subtraction or unkeyed stdout/mempool block join.",
    }

    if proposal_timings:
        result.update(
            matched_committed_proposal_timings=len(joined_timings),
            unjoined_proposal_timings=[
                dict(height=height, value_prefix=value, **timing)
                for (height, value), timing in sorted(proposal_timings.items())
                if (height, value) not in joined_timings
            ],
        )
    if reply_stats:
        result["proposal_reply_stats"] = list(reply_stats.values())
    if large_receipts:
        result["proposal_receive"] = list(large_receipts.values())
    return result


def snapshot_lines(stream, size):
    max_line = 1 << 20
    while size:
        line = stream.readline(min(size, max_line + 1))
        if not line:
            raise ValueError("typed log shrank during collection")
        if len(line) > max_line:
            raise ValueError("typed log line exceeds one MiB")
        size -= len(line)
        yield line.decode("utf-8", errors="replace")


def check_reply_scope(result, client, server, port):
    expected = str(IPv4Address(client)), str(IPv4Address(server)), int(port)
    rows = result.get("proposal_reply_stats")
    if not rows or any(
        (row["client_ip"], row["server_ip"], row["server_port"]) != expected
        for row in rows
    ):
        raise ValueError("proposal reply stats are missing or outside the hog4-to-hog10 scheduler connection scope")


def main(args):
    if len(args) == 4 and args[0] == "check-reply-stats":
        check_reply_scope(json.load(sys.stdin), *args[1:])
        return
    if len(args) != 2 or args[0] not in ("stdout", "stderr"):
        raise ValueError("expected stdout or stderr and snapshot size")
    stream, size = args[0], int(args[1])
    limit = (1 if stream == "stdout" else 256) << 20
    if not 0 < size <= limit:
        raise ValueError("typed log snapshot size is outside bounds")
    lines = snapshot_lines(sys.stdin.buffer, size)
    if stream == "stdout":
        print(normalize_stdout(lines), end="")
    else:
        print(json.dumps(reduce_stderr(lines), indent=2))


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except ValueError as error:
        sys.exit(f"ASAN coordinator diagnostic: {error}")
