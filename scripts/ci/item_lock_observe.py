#!/usr/bin/env python3
"""Timestamp the original benchmark's existing log messages externally."""
import os
import re
import sys
import time
from pathlib import Path

pid = int(sys.argv[1])
path = Path(sys.argv[2])
output = Path(sys.argv[3])
seen = 0
pending = ""
trial = None
start = re.compile(r"bench rep (\d+) for (\d+) txs .* on (\d+) threads")
finish = re.compile(r"bench rep tps: ([\d.eE+-]+)")
with output.open("w", buffering=1) as out:
    while True:
        if path.exists():
            with path.open() as stream:
                stream.seek(seen)
                pending += stream.read()
                seen = stream.tell()
            lines = pending.split("\n")
            pending = lines.pop()
            for line in lines:
                now = time.monotonic_ns()
                if match := start.search(line):
                    rep, txs, threads = map(int, match.groups())
                    trial = dict(rep=rep, txs=txs, threads=threads, start_ns=now)
                elif trial is not None and (match := finish.search(line)):
                    trial["end_ns"] = now
                    trial["time_us"] = round(trial["txs"] * 1e6 / float(match[1]))
                    print("LOCKTRIAL " + " ".join(f"{k}={v}" for k, v in trial.items()), file=out)
                    trial = None
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.002)
