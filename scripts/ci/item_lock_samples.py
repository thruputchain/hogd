#!/usr/bin/env python3
import bisect
import collections
import json
import re
import sys
from pathlib import Path

trials = []
if sys.argv[1]:
    for line in Path(sys.argv[1]).read_text().splitlines():
        if "LOCKTRIAL" in line:
            row = dict(re.findall(r"(\w+)=(\S+)", line))
            trials.append(row)
starts = [int(t["start_ns"]) for t in trials]
groups = collections.defaultdict(collections.Counter)
counts = collections.Counter()
for line in sys.stdin:
    match = re.match(r"\s*(\d+\.\d+):?\s+(.*)", line)
    if not match:
        continue
    seconds, symbol = match.groups()
    timestamp = round(float(seconds) * 1e9)
    if trials:
        index = bisect.bisect_right(starts, timestamp) - 1
        if index < 0 or timestamp > int(trials[index]["end_ns"]):
            continue
        t = trials[index]
        key = f"threads={t['threads']} rep={t['rep']} us={t['time_us']}"
    else:
        key = f"second={int(float(seconds))}"
    groups[key][symbol] += 1
    counts[key] += 1
json.dump({key: {"samples": counts[key], "symbols": count.most_common(30)}
           for key, count in groups.items()}, sys.stdout, indent=2)
print()
