#!/usr/bin/env python3
"""Verify complete Event CPU run evidence and summarize the four measured arms."""
import json
from pathlib import Path
import re
from statistics import median
import sys


def load(path):
    return json.loads(path.read_text())


def checked(condition, message):
    if not condition:
        raise SystemExit(message)


def verify(root):
    topology = load(root / "topology.json")
    network = load(root / "hog11-network.json")
    packed = tuple(topology["mask"])
    checked(len(packed) == 24 and len(set(packed)) == 24, "wrong packed CPU mask")
    settings = (root / "settings.txt").read_text()
    commit, = re.findall(r"^source=([a-f0-9]{40})$", settings, re.M)
    rows = []
    expected = re.compile(
        r"^sent 1000000 events in (\d+) ns \(([\d.]+) inner events/sec\) "
        r"opened block in (\d+) ns closed block in (\d+) ns "
        r"total time: (\d+) ns \(([\d.]+) total events/sec\)$", re.M
    )
    stock = None
    client_width = None
    pids = set()
    for index, arm in enumerate(("stock", "packed", "packed", "stock"), 1):
        tag = f"{index}-{arm}"
        before, after = [load(root / f"{tag}.{when}.json") for when in ("before", "after")]
        checked(before["pid"] == after["pid"] and before["pid"] not in pids,
                f"{tag}: unexpected process changes")
        pids.add(before["pid"])
        masks = set()
        for proof in (before, after):
            checked(proof["network"] == network and proof["filesystem"] == "tmpfs"
                    and proof["mode"] == arm and proof["source_sha"] == commit,
                    f"{tag}: wrong role, source, storage or endpoint")
            checked(len(proof["tids"]) >= proof["pool_width"], f"{tag}: missing worker evidence")
            masks.update(tuple(cpus) for cpus in proof["tids"].values())
        checked(len(masks) == 1, f"{tag}: affinity changed")
        mask, = masks
        checked(len(mask) == before["pool_width"] == after["pool_width"], f"{tag}: pool width differs")
        if arm == "packed":
            checked(mask == packed, f"{tag}: wrong packed affinity")
        elif stock is None:
            stock = mask
            checked(set(packed) < set(stock), "stock mask is not wider")
        else:
            checked(mask == stock, f"{tag}: wrong repeated stock affinity")

        stderr = (root / f"{tag}.client.stderr").read_text()
        checked(re.findall(r"git commit hash: ([a-f0-9]{40})", stderr) == [commit],
                f"{tag}: Event client source differs")
        width, = re.findall(r"pid:\d+, os:Linux, arch:[^,]+, cores:(\d+)", stderr)
        if client_width is None:
            client_width = int(width)
        checked(client_width > 0 and int(width) == client_width,
                f"{tag}: Event client width changed")
        text = (root / f"{tag}.client.log").read_text()
        samples = expected.findall(text)
        checked(len(samples) == 20 and len(re.findall(r"^sent ", text, re.M)) == 20,
                f"{tag}: wrong sample count or workload")
        checked(all(0 < int(r[0]) <= int(r[4]) and
                    0 < float(r[5]) <= float(r[1]) for r in samples),
                f"{tag}: invalid timing or throughput")
        rows.append({"arm": arm, "ordinal": index, "cores": len(mask), "samples": len(samples),
                     "client_cores": client_width, "send_ns_p50": median(int(r[0]) for r in samples),
                     "close_ns_p50": median(int(r[3]) for r in samples),
                     "total_events_s_p50": median(float(r[5]) for r in samples)})
    return rows


if __name__ == "__main__":
    print(json.dumps(verify(Path(sys.argv[1])), indent=2))
