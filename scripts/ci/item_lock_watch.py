#!/usr/bin/env python3
import os
import re
import sys
import time
from pathlib import Path

pid = int(sys.argv[1])
path = Path(sys.argv[2])
seen = 0
threshold = int(sys.argv[3]) if len(sys.argv) > 3 else 400000
pending = ""
while True:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        sys.exit(1)
    if path.exists():
        with path.open() as stream:
            stream.seek(seen)
            data = stream.read()
            seen = stream.tell()
        pending += data
        lines = pending.split("\n")
        pending = lines.pop()
        for line in lines:
            if "LOCKTRIAL" not in line:
                continue
            vals = dict(re.findall(r"(\w+)=(\S+)", line))
            if (vals.get("threads") == "2048"
                    and vals.get("txs", "100000") == "100000"
                    and int(vals["time_us"]) > threshold):
                print(line, flush=True)
                sys.exit(0)
    time.sleep(0.05)
