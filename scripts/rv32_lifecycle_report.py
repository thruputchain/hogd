#!/usr/bin/env python3
"""Report lifecycle rows from rv32_syscall_entry_bench in ns and opcode gas.

These are inclusive diagnostic costs, not additive gas-schedule entries.
"""
import argparse
import math
import re

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
        name = re.sub(r"/(?:iterations:\d+|manual_time)(?=/|$)", "", name)
        result[name] = value * scale
    isolated = any(isolated_row(name) for name in result)
    if isolated:
        if data.get("context", {}).get("lifecycle_timing_version") != "2":
            raise ValueError("obsolete pause/resume lifecycle timings; rerun the benchmark")
        if "lifecycle/TimerFloor" not in result:
            raise ValueError("missing lifecycle/TimerFloor; include it in the benchmark filter")
    return result


def isolated_row(name):
    return name.split("/")[1] in {
        "Setup", "Teardown", "SetupPoolMiss", "SetupTeardownBatch"}


def gas_estimate(name, ns, measured, unit):
    # A clock/loop floor that is over 10% of the observation makes an isolated
    # price unreliable. Keep the inclusive timing visible, never subtract it.
    if name == "lifecycle/TimerFloor":
        return "timer floor"
    if isolated_row(name) and ns < 10 * measured["lifecycle/TimerFloor"]:
        return "unresolved"
    return str(math.ceil(ns / unit)) if unit else ""


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
        print(f"{'row':58} {'wall ns':>12} {'gas / status':>15}")
        for name, ns in sorted(measured.items()):
            estimate = gas_estimate(name, ns, measured, unit)
            print(f"{name:58} {ns:12.2f} {estimate:>15}")
        print("Unresolved = less than 10x timer floor; no gas estimate. Timings include the floor.")
    except (ValueError, OSError) as error:
        parser.exit(1, f"rv32_lifecycle_report: {error}\n")


if __name__ == "__main__":
    main()
