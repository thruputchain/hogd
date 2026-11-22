#!/usr/bin/env python3
"""Reduce hog4 coordinator logs inside their own container."""

from calendar import timegm
from collections import Counter, defaultdict
import json
import re
from statistics import median
import sys
from time import strptime

from nightly_single_stage import COORDINATOR_BLOCK, COORDINATOR_THROUGHPUT

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
PHASES = {
    "nomination_start_to_prepersist_wait": ("nomination_start", "prepersist_wait"),
    "prepersist_caller_wait": ("prepersist_wait", "prepersist_done"),
    "prepersist_done_to_externalized": ("prepersist_done", "externalized"),
    "nomination_info_to_nominated_info": ("nomination_start", "nomination_done"),
    "externalized_to_commit_entry": ("externalized", "commit_start"),
    "commit_entry_to_catchup_persist_wait": ("commit_start", "persist_wait"),
    "catchup_persist_wait_to_commit_done": ("persist_wait", "commit_done"),
    "commit_entry_to_commit_done": ("commit_start", "commit_done"),
}


def normalize_stdout(lines):
    duration = None
    output = []
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if duration is None and COORDINATOR_BLOCK.fullmatch(line):
            duration = line
        elif duration is not None and COORDINATOR_THROUGHPUT.fullmatch(line):
            output.extend((duration, line))
            duration = None
        else:
            raise ValueError("unexpected coordinator stdout shape")
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
    for raw in lines:
        line = ANSI.sub("", raw)
        clock = STAMP.match(line)
        if not clock:
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
        if sum(matched.values()) > 100_000:
            raise ValueError("too many recognized coordinator INFO records")

    full = {key: events for key, events in blocks.items()
            if any(n == 1_000_000 for _, n in events["commit_done"])}
    if not full:
        raise ValueError("no committed one-million-transaction INFO records")

    distributions = defaultdict(list)
    waiting = Counter()
    for (height, value), events in full.items():
        times = {kind: items[0][0] for kind, items in events.items() if len(items) == 1}
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
        for phase, (start, end) in PHASES.items():
            if start in times and end in times and times[end] >= times[start]:
                distributions[phase].append((times[end] - times[start]) / 1e6)

    def report(values):
        result = {"samples": len(values), "unobserved": len(full) - len(values)}
        if values:
            values.sort()
            result.update(p50_ms=median(values), p90_ms=values[(len(values)-1)*9//10])
        return result

    return {
        "full_committed_blocks": len(full),
        "matched_info_records": matched,
        "prepersist_caller_outcomes": {
            "finished": waiting["finished"],
            "not_needed": waiting["not_needed"],
            "unobserved": len(full) - sum(waiting.values()),
        },
        "nominal_phases": {name: report(distributions[name]) for name in PHASES},
        "clock": "Single-process nominal UTC log timestamps.",
        "limitations": "INFO exposes caller waiting, not actual storage operation boundaries. "
                       "Catchup-persist wait to commit-done includes the scheduler commit RPC.",
    }


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["stdout"]:
            print(normalize_stdout(sys.stdin), end="")
        elif sys.argv[1:] == ["stderr"]:
            print(json.dumps(reduce_stderr(sys.stdin), indent=2))
        else:
            raise ValueError("expected stdout or stderr")
    except ValueError as error:
        sys.exit(f"Coordinator storage diagnostic: {error}")
