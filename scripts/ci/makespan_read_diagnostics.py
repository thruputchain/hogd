#!/usr/bin/env python3
"""Join sampled makespan read-RPC dumps and report stage durations by level."""

import argparse
from collections import defaultdict
import json
from pathlib import Path


def read_rows(path, nfields):
    with path.open() as source:
        for line_number, line in enumerate(source, 1):
            try:
                row = tuple(map(int, line.split()))
                if len(row) != nfields or any(value < 0 for value in row):
                    raise ValueError("invalid record shape")
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            yield row


def percentiles(values):
    values = sorted(values)
    return {
        "p50_us": values[(len(values) - 1) * 50 // 100] / 1_000,
        "p99_us": values[(len(values) - 1) * 99 // 100] / 1_000,
    }


def compare(client_path, server_path):
    client_by_key = defaultdict(list)
    level_config = {}
    reads_by_level = defaultdict(set)
    last_level = -1
    probe_block = None
    for level, inflight, connections, block, read, index, rtt in read_rows(
        client_path, 7
    ):
        if level < last_level or level > last_level + 1:
            raise ValueError(f"unexpected client level {level} after {last_level}")
        last_level = level
        if level_config.setdefault(level, (inflight, connections)) != (
            inflight,
            connections,
        ):
            raise ValueError(f"inconsistent client configuration for level {level}")
        if probe_block is None:
            probe_block = block
        if block != probe_block:
            raise ValueError("client records span multiple probe blocks")
        if index >= inflight or not rtt or not (
            0 < read <= 1_000_000
            and ((index == 0 and read <= 2_000) or read % 1024 == 0)
        ):
            raise ValueError(f"invalid client sample for level {level}: {read}/{index}")
        if read in reads_by_level[level]:
            raise ValueError(f"duplicate read {read} in level {level}")
        reads_by_level[level].add(read)
        client_by_key[read, index].append((level, rtt))

    base = level_config.get(1, (0, 0))[0]
    expected_levels = [
        (1, 1),
        (base, base),
        (10 * base, base),
        (30 * base, base),
        (100 * base, base),
        (100 * base, 2 * base),
        (100 * base, 4 * base),
        (100 * base, 8 * base),
        (30 * base, 4 * base),
    ]
    if not base or list(level_config.values()) != expected_levels:
        raise ValueError("missing or unexpected makespan probe levels")

    server_by_key = defaultdict(list)
    for block, read, index, received, entry, handler, residence in read_rows(
        server_path, 7
    ):
        if block != probe_block:
            continue
        if not received or entry + handler > residence:
            raise ValueError(f"invalid server stages for {read}/{index}")
        server_by_key[read, index].append((received, entry, handler, residence))
    if server_by_key.keys() != client_by_key.keys():
        missing = len(client_by_key.keys() - server_by_key.keys())
        extra = len(server_by_key.keys() - client_by_key.keys())
        raise ValueError(f"server and client sample keys differ ({missing=} {extra=})")

    results = defaultdict(list)
    for key, client_records in client_by_key.items():
        server_records = sorted(server_by_key[key])
        if len(client_records) != len(server_records):
            raise ValueError(f"sample count differs for {key}")
        if len({received for received, *_ in server_records}) != len(server_records):
            raise ValueError(f"server receive clock cannot order occurrences of {key}")
        for (level, rtt), (_, entry, handler, residence) in zip(
            client_records, server_records, strict=True
        ):
            results[level].append(
                (rtt, entry, handler, residence - entry - handler,
                 residence, rtt - residence)
            )

    stage_names = (
        "client_rtt",
        "server_record_read_to_handler",
        "server_handler",
        "server_handler_to_reply_attempt",
        "server_record_read_to_reply_attempt",
        "client_rtt_minus_server_residence",
    )
    summary = {}
    for level, (inflight, connections) in level_config.items():
        rows = results[level]
        sampled = reads_by_level[level]
        if inflight == 1 and sampled != set(range(1, 2_001)):
            raise ValueError(f"serial level {level} does not contain all 2,000 reads")
        budget = 2_000 if inflight == 1 else 300_000 if inflight <= base else 1_000_000
        periodic = {read for read in sampled if read % 1024 == 0}
        if periodic != set(range(1024, budget + 1, 1024)):
            raise ValueError(f"missing or unexpected periodic samples for level {level}")
        summary[level] = {
            "inflight": inflight,
            "connections": connections,
            "samples": len(rows),
            "negative_residual_samples": sum(row[-1] < 0 for row in rows),
            **{
                stage: percentiles(values)
                for stage, values in zip(stage_names, zip(*rows), strict=True)
            },
        }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("client_dump", type=Path)
    parser.add_argument("server_dump", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(compare(args.client_dump, args.server_dump), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
