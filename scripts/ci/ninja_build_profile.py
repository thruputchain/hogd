#!/usr/bin/env python3
"""Summarize where a Ninja build spent its time, from <build-dir>/.ninja_log.

CI prints this after each build so a slow build shows whether the time went
to C++ compiles (ccache misses), links, the Rust/RV32 contract builds, or
something else, without downloading the log.

Usage: scripts/ci/ninja_build_profile.py <build-dir> [top-n]
"""

import sys
from collections import defaultdict
from pathlib import Path


def category(outputs):
    if any(out.endswith(".rv32a") for out in outputs):
        return "rv32 contracts"
    if any("cargo" in out or "corrosion" in out for out in outputs):
        return "rust"
    path = outputs[0]
    if path.endswith((".o", ".obj")):
        return "compile"
    if path.endswith((".a", ".so", ".dylib")) or "." not in Path(path).name:
        return "link"
    return "other"


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    log = Path(sys.argv[1]) / ".ninja_log"
    top_n = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    if not log.is_file():
        print(f"no ninja log at {log}")
        return 0

    # Lines are "start_ms end_ms mtime output cmdhash"; outputs of one edge
    # share start, end and hash. A later entry for an output supersedes an
    # earlier one, as when ninja itself reads the log.
    latest = {}
    for line in log.read_text(errors="replace").splitlines():
        fields = line.split("\t")
        if line.startswith("#") or len(fields) < 4:
            continue
        try:
            start, end = int(fields[0]), int(fields[1])
        except ValueError:
            continue
        cmdhash = fields[4] if len(fields) > 4 else ""
        latest[fields[3]] = (start, end, cmdhash)
    edges = defaultdict(list)
    for output, key in latest.items():
        edges[key].append(output)
    if not edges:
        print(f"{log} has no entries")
        return 0

    totals = defaultdict(lambda: [0, 0.0])
    for (start, end, _), outputs in edges.items():
        total = totals[category(sorted(outputs))]
        total[0] += 1
        total[1] += (end - start) / 1000
    first = min(start for start, _, _ in edges)
    wall = (max(end for _, end, _ in edges) - first) / 1000
    summed = sum(seconds for _, seconds in totals.values())
    print(f"{log}: {len(edges)} edges, {wall:.0f}s wall, {summed:.0f}s summed")
    by_time = sorted(totals.items(), key=lambda kv: -kv[1][1])
    for name, (count, seconds) in by_time:
        print(f"  {name:15s} {count:5d} edges {seconds:8.0f}s")
    print(f"Slowest {top_n} edges (seconds, start..end from build start):")
    by_duration = sorted(edges.items(), key=lambda kv: kv[0][0] - kv[0][1])
    for (start, end, _), outputs in by_duration[:top_n]:
        outputs = sorted(outputs)
        more = f" (+{len(outputs) - 1} more)" if len(outputs) > 1 else ""
        span = f"{(start - first) / 1000:.0f}..{(end - first) / 1000:.0f}"
        print(f"  {(end - start) / 1000:7.1f}s  {span:>11s}  {outputs[0]}{more}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
