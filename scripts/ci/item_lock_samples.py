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
            trials.append(dict(re.findall(r"(\w+)=(\S+)", line)))
starts = [int(t["start_ns"]) for t in trials]
leaves = collections.defaultdict(collections.Counter)
stacks = collections.defaultdict(collections.Counter)
timestamp = None
frames = []


def frame(text):
    text = re.sub(r"^\s*[0-9a-fA-F]+\s+", "", text.strip())
    return re.sub(r"\+0x[0-9a-fA-F]+", "", text)


def flush():
    if timestamp is None or not frames:
        return
    if trials:
        index = bisect.bisect_right(starts, timestamp) - 1
        if index < 0:
            return
        t = trials[index]
        # The external observer polls every 2ms; exclude the uncertain edges.
        if timestamp < int(t["start_ns"]) + 3000000 or timestamp > int(t["end_ns"]) - 3000000:
            return
        key = f"threads={t['threads']} txs={t.get('txs', '?')} rep={t['rep']} us={t['time_us']}"
    else:
        key = f"second={timestamp // 1000000000}"
    unique = [x for i, x in enumerate(frames) if i == 0 or x != frames[i - 1]]
    leaves[key][unique[0]] += 1
    stacks[key][" <- ".join(unique[:12])] += 1


for line in sys.stdin:
    match = re.match(r"\s*(\d+)\.(\d+):\s*(.*)", line)
    if match:
        flush()
        seconds, frac, tail = match.groups()
        timestamp = int(seconds) * 1000000000 + int(frac.ljust(9, "0")[:9])
        frames = [frame(tail)] if tail.strip() else []
    elif timestamp is not None and re.match(r"\s*[0-9a-fA-F]+\s+", line):
        frames.append(frame(line))
    elif not line.strip():
        flush()
        timestamp = None
        frames = []
flush()
json.dump({key: {"samples": sum(count.values()), "symbols": count.most_common(30),
                 "stacks": stacks[key].most_common(30)}
           for key, count in leaves.items()}, sys.stdout, indent=2)
print()
