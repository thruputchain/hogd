#!/usr/bin/env python3
"""Summarize replay accounting, latency and same-host ingestion/block overlap."""
import collections
import bisect
import json
import math
from pathlib import Path
import re
import sys


def quantiles(values):
    values = sorted(values)
    return {f"p{p}": values[min(len(values)-1, math.ceil(p/100*len(values))-1)] if values else None for p in (50, 95, 99)}


def lines(path):
    with path.open() as stream:
        yield from stream


def summarize(directory):
    phases = collections.defaultdict(list)
    for line in lines(directory / "replay/results.jsonl"):
        row = json.loads(line)
        phases[row["phase"]].append(row)
    manifest = json.loads((directory / "corpus/manifest.json").read_text())
    report = {"phases": {}, "overlap": {}, "metricDeltas": {}}
    for phase, rows in sorted(phases.items()):
        duration = manifest["warmupSeconds"] if phase == 0 else manifest["stageSeconds"]
        start_ns = 0 if phase == 0 else (manifest["warmupSeconds"] + (phase-1)*manifest["stageSeconds"]) * 1e9
        end_ns = start_ns + duration * 1e9
        outcomes = collections.Counter(r["outcome"] for r in rows)
        execution = collections.Counter(r.get("execution", "unresolved") for r in rows if r["startNs"] > 0)
        submitted = [r for r in rows if r["startNs"] > 0]
        completed = [r for r in submitted if r["endNs"] > 0]
        report["phases"][str(phase)] = {
            "offered": len(rows), "submitted": len(submitted), "outcomes": outcomes,
            "execution": execution, "offeredPerSecond": len(rows)/duration,
            "admittedWithinWindowPerSecond": sum(r["outcome"] == "admitted" and start_ns <= r["endNs"] < end_ns for r in rows)/duration,
            "observedCommittedWithinWindowPerSecond": sum(start_ns <= r.get("receiptNs", 0) < end_ns and r.get("receiptNs", 0) > 0 for r in rows)/duration,
            "rpcLatencyMs": quantiles([(r["endNs"]-r["startNs"])/1e6 for r in completed]),
            "arrivalDelayMs": quantiles([(r["startNs"]-r["dueNs"])/1e6 for r in submitted]),
            "receiptObservationMs": quantiles([(r["receiptNs"]-r["startNs"])/1e6 for r in rows if r.get("receiptNs",0)>0]),
        }
    intervals = collections.defaultdict(list)
    run_path = directory / "replay/run.json"
    run = json.loads(run_path.read_text()) if run_path.exists() else {}
    window_start = run.get("startedUnixNs", 0)
    window_end = window_start + manifest.get("durationSeconds", 0) * 1_000_000_000
    pattern = re.compile(r"INGESTION_INTERVAL phase=(\w+) start_ns=(\d+) end_ns=(\d+)(?: unix_start_ns=(\d+))?")
    for path in directory.glob("mempool.*.log"):
        for match in pattern.finditer(path.read_text(errors="replace")):
            # Wall time selects the client measurement window; overlap itself
            # uses only the mempool's monotonic clock. Keep raw intervals for
            # investigation of clock skew on remote hosts.
            if match[4] and window_start:
                begin = int(match[4])
                end = begin + int(match[3])-int(match[2])
                if match[1].startswith("submit"):
                    if not window_start <= begin < window_end:
                        continue
                elif end <= window_start or begin >= window_end:
                    continue
            intervals[match[1]].append((int(match[2]), int(match[3])))
    submissions = intervals["submit_gateway"]
    for phase in ("propose", "validate", "commit"):
        blocks = sorted(intervals[phase])
        merged = []
        for begin, end in blocks:
            if merged and begin <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], end))
            else:
                merged.append((begin, end))
        starts = [begin for begin, _ in merged]
        overlap, independent = [], []
        for a, b in submissions:
            index = bisect.bisect_left(starts, b)-1
            intersects = index >= 0 and merged[index][1] > a
            (overlap if intersects else independent).append((b-a)/1e6)
        report["overlap"][phase] = {"blockIntervals": len(blocks), "sampledOverlappingSubmissions": len(overlap), "overlapLatencyMs": quantiles(overlap), "otherLatencyMs": quantiles(independent), "exercised": bool(blocks)}
    snapshots = collections.defaultdict(list)
    metrics_path = directory / "replay/metrics.jsonl"
    for line in lines(metrics_path):
        sample = json.loads(line)
        if "text" in sample:
            values = {}
            for metric in sample["text"].splitlines():
                if not metric or metric.startswith("#"):
                    continue
                parts = metric.rsplit(" ",1)
                try:
                    values[parts[0]] = float(parts[1])
                except (ValueError, IndexError):
                    continue
            snapshots[sample["name"]].append(values)
    for name, samples in snapshots.items():
        first, last = samples[0], samples[-1]
        report["metricDeltas"][name] = {key: value-first.get(key,value) for key,value in last.items() if key.endswith(("_sum","_count","_total")) or "_bucket{" in key}
    for name, deltas in report["metricDeltas"].items():
        if name != "mempool":
            continue
        histograms = {}
        for key, count in deltas.items():
            match = re.fullmatch(r'(mempool_(?:ingestion|block_commit_lock|scheduler_global_lock)[a-z_]+)_bucket\{le="([^"]+)"\}', key)
            if match:
                histograms.setdefault(match[1], []).append((float(match[2]), count))
        report["lockAndAuthHistogramQuantileUpperBoundsSeconds"] = {}
        for name, buckets in histograms.items():
            buckets.sort()
            total = buckets[-1][1]
            report["lockAndAuthHistogramQuantileUpperBoundsSeconds"][name] = {
                f"p{p}": next((bound if math.isfinite(bound) else "above-largest-finite-bucket" for bound, count in buckets if total and count >= total*p/100), None)
                for p in (50,95,99)
            }
    (directory / "summary.json").write_text(json.dumps(report, indent=2)+"\n")
    text = ["# Hackathon replay", "", "All application traffic uses the RPC gateway. Receipt times include polling/indexer delay.", "", "| Phase | Offered | Sent | RPC p99 ms | Execution outcomes |", "|---|---:|---:|---:|---|"]
    for phase, data in report["phases"].items():
        text.append(f"| {phase} | {data['offered']} | {data['submitted']} | {data['rpcLatencyMs']['p99']} | {dict(data['execution'])} |")
    text.extend(["", "Sampled ingestion overlap with block processing:", ""])
    for phase, data in report["overlap"].items():
        text.append(f"- {phase}: {data['sampledOverlappingSubmissions']} overlapping submissions across {data['blockIntervals']} intervals." if data["exercised"] else f"- {phase}: not exercised by this run.")
    text.extend(["", "Inspect authentication and lock wait/hold histogram deltas in summary.json alongside the raw metric time series. Overlap alone is correlation, not proof of lock contention."])
    (directory / "report.md").write_text("\n".join(text)+"\n")
    if sum(len(rows) for rows in phases.values()) != manifest["operations"]:
        raise ValueError("incomplete result accounting")
    if not submissions or not intervals["commit"] or len(snapshots.get("mempool",[])) < 2:
        raise ValueError("missing ingestion/commit diagnostics or mempool telemetry")
    return report


if __name__ == "__main__":
    summarize(Path(sys.argv[1]))
