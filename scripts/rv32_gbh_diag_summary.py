#!/usr/bin/env python3
"""Summarize one rv32_gbh_diag.sh run. Diagnostic branch only, not for merge.

Prints one line per benchmark row (median ns per call: ns_per_call for VM
rows, cpu_time for entry rows), then the distinct address dumps the run
printed, if any.

usage: rv32_gbh_diag_summary.py LABEL JSON STDERR EXIT_CODE
"""

import json
import sys


def main() -> int:
    label, json_path, err_path, rc = sys.argv[1:5]
    try:
        with open(json_path) as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print(f"gbh-diag {label}: rc={rc}, no JSON ({e})")
        try:
            with open(err_path, errors="replace") as f:
                tail = f.read().splitlines()[-15:]
            for line in tail:
                print(f"gbh-diag {label}:   {line}")
        except OSError:
            pass
        return 0

    medians = {}
    errors = []
    for row in data.get("benchmarks", []):
        if row.get("error_occurred"):
            errors.append(f"{row.get('name')}: {row.get('error_message')}")
            continue
        if row.get("aggregate_name") != "median":
            continue
        name = row["name"][: -len("_median")]
        if "ns_per_call" in row:
            medians[name] = row["ns_per_call"] * 1e9
        else:
            medians[name] = row["cpu_time"]
    cells = "  ".join(f"{name}={ns:.1f}" for name, ns in medians.items())
    print(f"gbh-diag {label}: rc={rc} {cells}")
    for error in errors:
        print(f"gbh-diag {label}: ERROR {error}")

    # Distinct address dumps per (where, row), with how often each appeared.
    seen = {}
    try:
        with open(err_path, errors="replace") as f:
            for line in f:
                if not line.startswith("gbh-diag-addr "):
                    continue
                fields = dict(
                    kv.split("=", 1) for kv in line.split()[1:] if "=" in kv
                )
                key = (fields.get("where"), fields.get("row"))
                body = line.strip()[len("gbh-diag-addr ") :]
                bucket = seen.setdefault(key, {})
                bucket[body] = bucket.get(body, 0) + 1
    except OSError:
        pass
    for (where, row), bodies in seen.items():
        for body, count in list(bodies.items())[:4]:
            print(f"gbh-diag {label}: addr x{count} {body}")
        if len(bodies) > 4:
            print(f"gbh-diag {label}: addr ... {len(bodies) - 4} more for "
                  f"{where} {row}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
