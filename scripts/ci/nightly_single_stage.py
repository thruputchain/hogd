#!/usr/bin/env python3
"""Summarize matched mempool stage durations and independent coordinator blocks."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
from statistics import median

STAGES = {
    1: "nomination",
    2: "validation",
    3: "commit",
    4: "db_commit_rpc",
    5: "event_commit_rpc",
    6: "sampled_ingestion_auth_rpc",
}
FULL_BLOCK = 1_000_000
COORDINATOR_BLOCK = re.compile(
    r"Block duration \(s\), ([0-9]+(?:\.[0-9]+)?), Num txs in block, ([0-9]+), "
    r"Overall duration \(s\), ([0-9]+(?:\.[0-9]+)?), Overall number of txs, ([0-9]+)"
)


COORDINATOR_THROUGHPUT = re.compile(
    r"Block throughput \(tx/s\), [0-9]+(?:\.[0-9]+)?, "
    r"Overall throughput \(tx/s\), [0-9]+(?:\.[0-9]+)?"
)


def load_coordinator(path, artifact=None):
    rows = []
    invalid_line = None
    pending_duration = None
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.strip()
        if not text:
            continue
        match = COORDINATOR_BLOCK.fullmatch(text)
        if match and pending_duration is None:
            pending_duration = (number, match.groups())
        elif COORDINATOR_THROUGHPUT.fullmatch(text) and pending_duration is not None:
            _, fields = pending_duration
            rows.append(fields)
            pending_duration = None
        else:
            invalid_line = number
            break

    if artifact is not None:
        # Copy only recognized numeric records, even when the input is
        # incomplete. Leave the original report and mempool dump untouched.
        artifact.write_text("".join("\t".join(fields) + "\n" for fields in rows))
    if invalid_line is not None:
        raise ValueError(f"unrecognized coordinator report record on line {invalid_line}")
    if pending_duration is not None:
        raise ValueError(f"coordinator report ends after duration on line {pending_duration[0]}")
    full = [float(duration) * 1e3 for duration, count, _, _ in rows
            if int(count) == FULL_BLOCK]
    if not full:
        raise ValueError("no full 1,000,000-transaction coordinator blocks captured")
    return full


def distribution(values):
    if not values:
        return {"samples": 0}
    values = sorted(values)
    return {
        "samples": len(values),
        "p50_ms": median(values),
        "p90_ms": values[(len(values) - 1) * 90 // 100],
        "max_ms": values[-1],
    }


def load_stages(path):
    blocks = defaultdict(lambda: defaultdict(list))
    auth_samples = []
    outcomes = defaultdict(Counter)
    for number, line in enumerate(path.read_text().splitlines(), 1):
        try:
            stage, height, block, count, outcome, duration, end = map(int, line.split("\t"))
        except ValueError as error:
            raise ValueError(f"invalid numeric stage record on line {number}") from error
        if (stage not in STAGES or block < 0 or count < 0
                or duration < 0 or end < duration or (not block and stage not in (2, 6))):
            raise ValueError(f"invalid stage record on line {number}")
        outcomes[stage][outcome] += 1
        if stage == 6:
            if block or outcome:
                raise ValueError(f"invalid Auth completion on line {number}")
            auth_samples.append((end - duration, end, duration / 1e6))
        elif block and outcome == 0:
            blocks[height, block][stage].append((count, duration, end))
    return blocks, auth_samples, outcomes


def summarize(stage_path, coordinator_path, coordinator_artifact=None):
    coordinator = load_coordinator(coordinator_path, coordinator_artifact)
    blocks, auth_samples, outcomes = load_stages(stage_path)
    durations = defaultdict(list)
    rows = []
    unaligned = []
    seen_heights = set()
    for (height, block), stages in sorted(blocks.items()):
        commits = stages[3]
        if not any(count == FULL_BLOCK for count, _, _ in commits):
            continue
        if len(commits) != 1 or height in seen_heights:
            raise ValueError(f"ambiguous full commit at height {height}")
        seen_heights.add(height)
        count, commit_duration, commit_end = commits[0]
        records_by_stage = {}
        row = {"height": height, "block": block, "txs": count}
        starts = [commit_end - commit_duration]
        for stage in range(1, 6):
            records = stages[stage]
            if stage in (1, 2):
                records = [record for record in records if record[0] == count]
            if len(records) > 1:
                raise ValueError(f"ambiguous {STAGES[stage]} at height {height}")
            if records:
                _, duration, end = records[0]
                value = duration / 1e6
                row[STAGES[stage] + "_ms"] = value
                records_by_stage[stage] = value
                starts.append(end - duration)
        if (not (1 in records_by_stage or 2 in records_by_stage)
                or 4 not in records_by_stage or 5 not in records_by_stage):
            unaligned.append({"height": height, "block": block})
            continue
        for stage, value in records_by_stage.items():
            durations[stage].append(value)
        # Sampled requests may overlap more than one block's local activity.
        # These durations are not additive and do not imply transaction inclusion.
        beginning = min(starts)
        row["sampled_auth_intervals_overlapping"] = sum(
            end > beginning and start < commit_end for start, end, _ in auth_samples
        )
        rows.append(row)
    if not rows:
        raise ValueError("no full 1,000,000-transaction commits with aligned "
                         "nomination or validation and both commit RPCs")

    return {
        "mempool_full_blocks": rows,
        "unaligned_mempool_full_blocks": unaligned,
        "mempool_full_block_stage_distributions": {
            STAGES[stage]: distribution(durations[stage]) for stage in range(1, 6)
        },
        "sampled_ingestion_auth_rpc": distribution([row[2] for row in auth_samples]),
        "auth_sampling": "One completed Auth RPC in 64; duration is per RPC, not per transaction.",
        "outcomes_by_stage": {STAGES[stage]: counts for stage, counts in outcomes.items()},
        "independent_coordinator_full_blocks": distribution(coordinator),
        "coordinator_full_block_ms_in_report_order": coordinator,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mempool_records", type=Path)
    parser.add_argument("coordinator_report", type=Path)
    parser.add_argument("--coordinator-artifact", type=Path)
    parser.add_argument("--summary-output", type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.mempool_records, args.coordinator_report,
                           args.coordinator_artifact)
        report = json.dumps(result, indent=2) + "\n"
        if args.summary_output:
            args.summary_output.write_text(report)
        else:
            print(report, end="")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Nightly stage diagnostic: {error}\n")
