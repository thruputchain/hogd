#!/usr/bin/env python3
import os
import re
import sys
import time
from pathlib import Path

pid = int(sys.argv[1])
path = Path(sys.argv[2])
seen = 0
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
        for line in data.splitlines():
            if "LOCKTRIAL" not in line:
                continue
            vals = dict(re.findall(r"(\w+)=(\S+)", line))
            if vals.get("threads") == "2048" and int(vals["time_us"]) > 400000:
                print(line, flush=True)
                sys.exit(0)
    time.sleep(0.05)
