#!/usr/bin/env python3
"""Report lifecycle rows from rv32_syscall_entry_bench in ns and opcode gas.

These are inclusive diagnostic costs, not additive gas-schedule entries.
"""
import argparse
import math

from rv32_gas_units import DEFAULT_RESOLUTION, baseline_ns, gas_unit_ns
from rv32_syscall_report import check_joinable, failed_rows, load, rows


def lifecycle_rows(data):
    failed = {name: reason for name, reason in failed_rows(data).items()
              if name.startswith("lifecycle/")}
    if failed:
        raise ValueError("failed lifecycle rows: " + repr(failed))
    measured = {name: row for name, row in rows(data).items()
                if name.startswith("lifecycle/")}
    if not measured:
        raise ValueError("no lifecycle measurements")
    scales = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}
    result = {}
    for name, row in measured.items():
        scale = scales.get(row.get("time_unit"))
        value = row.get("real_time")
        if scale is None or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError(f"invalid wall-clock timing for {name}")
        result[name] = value * scale
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("benchmark")
    parser.add_argument("--gas", help="opcode calibration from the same host/build")
    args = parser.parse_args()
    try:
        bench = load(args.benchmark)
        measured = lifecycle_rows(bench)
        unit = None
        if args.gas:
            calibration = load(args.gas)
            problems = check_joinable(bench, calibration)
            # Lifecycle code sits outside the opcode fingerprint. Require the
            # same source revision and compiler/flags as well as host/engine.
            for key in ("source_revision", "compiler", "build_type", "cxx_flags"):
                left = bench.get("context", {}).get("hogd_" + key)
                right = calibration.get("provenance", {}).get(key)
                if not left or not right or left != right:
                    problems.append(f"missing or mismatched {key}")
            if problems:
                raise ValueError("; ".join(problems))
            unit = gas_unit_ns(baseline_ns(calibration), DEFAULT_RESOLUTION)
        print("Inclusive lifecycle costs; do not add Prepare, Setup, and Invoke rows together.")
        print(f"{'row':58} {'wall ns':>12}" + (f" {'gas equivalent':>15}" if unit else ""))
        for name, ns in sorted(measured.items()):
            print(f"{name:58} {ns:12.2f}" + (f" {math.ceil(ns / unit):15d}" if unit else ""))
    except (ValueError, OSError) as error:
        parser.exit(1, f"rv32_lifecycle_report: {error}\n")


if __name__ == "__main__":
    main()
