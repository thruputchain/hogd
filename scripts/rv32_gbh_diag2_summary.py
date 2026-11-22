#!/usr/bin/env python3
"""Summarize one rv32_gbh_diag2.sh run. Diagnostic branch only, not for merge.

Prints one line: the GetBlockHeight and GetCurrentAddress rows (median ns per
call), the vm/ gap between them, and, when the run dumped addresses, where
vm/GetBlockHeight's handler, guest memory, machine and stack landed. Writes the
gap alone to file descriptor 3 (nan if unmeasured), for the caller to branch
on.

usage: rv32_gbh_diag2_summary.py LABEL JSON STDERR EXIT_INFO
"""

import json
import os
import sys

ROWS = ["entry/GetBlockHeight", "vm/GetBlockHeight", "entry/GetCurrentAddress",
        "vm/GetCurrentAddress"]
ADDR_KEYS = ["handler", "handler_cache_off", "cache", "tip", "guest", "out_a",
             "machine", "gas", "exec", "stack"]


def emit_gap(gap):
    try:
        os.write(3, f"{gap}\n".encode())
    except OSError:
        pass


def main() -> int:
    label, json_path, err_path, info = sys.argv[1:5]
    try:
        with open(json_path) as f:
            rows = json.load(f)["benchmarks"]
    except (OSError, ValueError, KeyError) as e:
        print(f"gbh2 {label}: {info} no JSON ({e})")
        try:
            with open(err_path, errors="replace") as f:
                for line in f.read().splitlines()[-8:]:
                    print(f"gbh2 {label}:   {line}")
        except OSError:
            pass
        emit_gap("nan")
        return 0

    medians = {}
    errors = []
    for row in rows:
        if row.get("error_occurred"):
            errors.append(f"{row.get('name')}: {row.get('error_message')}")
        if row.get("aggregate_name") != "median":
            continue
        name = row["name"][: -len("_median")]
        medians[name] = (row["ns_per_call"] * 1e9 if "ns_per_call" in row
                         else row["cpu_time"])
    a = medians.get("vm/GetBlockHeight")
    b = medians.get("vm/GetCurrentAddress")
    gap = "nan" if a is None or b is None else f"{a - b:.1f}"
    cells = "  ".join(f"{name}={medians[name]:.1f}" for name in ROWS
                      if name in medians)
    addr = ""
    try:
        with open(err_path, errors="replace") as f:
            for line in f:
                if not line.startswith("gbh-diag-addr "):
                    continue
                fields = dict(kv.split("=", 1) for kv in line.split()[1:]
                              if "=" in kv)
                if fields.get("where") == "vm" and \
                        fields.get("row") == "GetBlockHeight":
                    addr = " | " + " ".join(f"{k}={fields[k]}"
                                            for k in ADDR_KEYS if k in fields)
                    break
    except OSError:
        pass
    print(f"gbh2 {label}: {info} {cells}  gap={gap}{addr}")
    for error in errors[:4]:
        print(f"gbh2 {label}: ERROR {error}")
    emit_gap(gap)
    return 0


if __name__ == "__main__":
    sys.exit(main())
