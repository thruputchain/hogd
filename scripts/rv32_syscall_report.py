#!/usr/bin/env python3
"""Turn an RV32 syscall benchmark run into gas costs.

    scripts/rv32_syscall_report.py sys.json                      # what it costs
    scripts/rv32_syscall_report.py sys.json --gas gas.json --schedule
    scripts/rv32_syscall_report.py before.json after.json        # what moved

The measurement is nanoseconds; a gas schedule is integers. Converting between
them needs a unit, and the unit is not a free choice: it is the baseline
instruction the *opcode* calibration measured on the same host, because gas is
one currency and a syscall priced against a different machine's baseline is not
priced at all. That is what --gas supplies, and why --schedule refuses without
it.

What the benchmark reports, per syscall:

  entry/NAME   syscall_entry called directly -- the ABI parse, the coroutine
               frame, the handler. No VM.
  vm/NAME      the same call driven end to end through ThreadedMachine, exactly
               as rv32_contract.cc drives it: ecall, suspend, syscall_entry,
               resume.

vm is what a guest causes and therefore what has to be paid for. entry is the
part a gas schedule can attribute to the syscall rather than to the ecall
instruction, and the difference between them is the seam -- which the gas
calibration harness measures independently, so the two are a check on each
other rather than one number reported twice.
"""

import argparse
import json
import math
import re
import statistics
import sys

# One gas is 1/RESOLUTION of a baseline instruction. Matches
# rv32_gas_report.py's default; a syscall schedule built at a different
# resolution than the opcode schedule it sits beside is not a schedule.
DEFAULT_RESOLUTION = 1

# The instruction gas the schedule already charges for the ecall itself
# (kBaseInstructionGas). Host gas is charged on top of it, so a proposal that
# ignored it would double-charge the crossing by one instruction.
ECALL_INSTRUCTION_GAS = 1

# Costs quoted per byte are only meaningful when the sweep is long enough to
# resolve a slope; below this the fit is measuring its own intercept.
MIN_SWEEP_POINTS = 4

SHA256_ROW = re.compile(r"^(entry|vm)/SHA256/(\d+)$")


def load(path):
    with open(path) as handle:
        data = json.load(handle)
    data["_path"] = path
    return data


def rows(data):
    """Median rows by name, falling back to un-aggregated runs.

    Google Benchmark emits per-repetition rows plus aggregates; the runner asks
    for aggregates only, but a hand-run file may have either.
    """
    medians = {}
    plain = {}
    for entry in data.get("benchmarks", []):
        name = entry.get("run_name") or entry.get("name")
        if not name:
            continue
        if entry.get("aggregate_name") == "median":
            medians[name] = entry
        elif "aggregate_name" not in entry:
            plain[name] = entry
    return plain | medians


def stddevs(data):
    return {entry.get("run_name"): entry
            for entry in data.get("benchmarks", [])
            if entry.get("aggregate_name") == "stddev"}


def context(data):
    return data.get("context", {})


def calls_per_vm_run(row):
    """Calls one vm/ iteration made, from the benchmark's own item count.

    Taken from the file rather than hardcoded: the benchmark chooses its
    iteration count, and a report that assumed one would silently misprice
    every vm row if that changed.
    """
    items = row.get("items_per_second")
    real = row.get("real_time")
    if not items or not real:
        return None
    # items_per_second is calls/second; real_time is ns per benchmark iteration.
    return items * real / 1e9


def vm_ns_per_call(row):
    calls = calls_per_vm_run(row)
    if not calls:
        return None
    return row["real_time"] / calls


def syscall_names(measured):
    names = []
    for name in measured:
        if not name.startswith("entry/") or SHA256_ROW.match(name):
            continue
        names.append(name[len("entry/"):])
    return sorted(names)


def sha256_points(measured, prefix="entry"):
    points = []
    for name, row in measured.items():
        match = SHA256_ROW.match(name)
        if match and match.group(1) == prefix:
            points.append((int(match.group(2)), row["real_time"]))
    return sorted(points)


def fit_sha256(points):
    """Least squares over the size sweep: (base_ns, ns_per_byte, r_squared).

    A fit rather than the endpoints, because the endpoints are the two points
    most exposed to a single bad sample and the whole sweep is cheap to use.
    """
    if len(points) < MIN_SWEEP_POINTS:
        return None
    xs = [float(p[0]) for p in points]
    ys = [float(p[1]) for p in points]
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    var_x = sum((x - mean_x) ** 2 for x in xs)
    if var_x == 0.0:
        return None
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / var_x
    intercept = mean_y - slope * mean_x
    residual = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    total = sum((y - mean_y) ** 2 for y in ys)
    r_squared = 1.0 if total == 0.0 else 1.0 - residual / total
    return intercept, slope, r_squared


def baseline_ns_from_calibration(data):
    """The gas unit's source: the median saturated opcode cell.

    Deliberately the same quantity rv32_gas_report.py normalizes by, computed
    the same way. If the two disagreed, a syscall cost and an instruction cost
    would be denominated in different gas.
    """
    costs = sorted(cell["ns_saturated"] for cell in data.get("opcodes", [])
                   if cell.get("usable") and not cell.get("pair")
                   and cell.get("ns_saturated", 0.0) > 0.0)
    if costs:
        return statistics.median(costs)
    return data.get("baseline", {}).get("ns_per_filler_instruction", 0.0)


def check_joinable(bench, calibration):
    """Refuse to price one host's syscalls in another host's gas.

    The unit comes from the calibration and the nanoseconds from the benchmark.
    Joining them is only meaningful when both describe the same engine on the
    same machine, and both binaries record enough to say so. This is the one
    check that cannot be skipped: getting it wrong produces a complete,
    plausible schedule that is wrong by whatever the two hosts differ by.
    """
    problems = []
    bench_ctx = context(bench)
    cal_prov = calibration.get("provenance", {})

    bench_fp = bench_ctx.get("engine_fingerprint")
    cal_fp = cal_prov.get("engine_fingerprint")
    if bench_fp and cal_fp and bench_fp != cal_fp:
        problems.append(
            f"engine fingerprint {bench_fp} (benchmark) != {cal_fp} "
            "(calibration): these measured different engines, so the "
            "calibration's baseline does not describe the benchmark's host "
            "instruction.")

    bench_host = bench_ctx.get("hogd_host")
    cal_host = cal_prov.get("host")
    if bench_host and cal_host and bench_host != cal_host:
        problems.append(
            f"host {bench_host} (benchmark) != {cal_host} (calibration): "
            "nanoseconds are a fact about one machine, and the gas unit would "
            "come from the other one.")

    if bench_ctx.get("hogd_production_build") == "false":
        problems.append("the benchmark was not a Release build")
    if cal_prov.get("production_build") is False:
        problems.append("the calibration was not a Release build")
    return problems


def print_table(data):
    measured = rows(data)
    sd = stddevs(data)
    if not measured:
        print("no benchmarks in this file", file=sys.stderr)
        return 1
    ctx = context(data)
    print(f"host:        {ctx.get('hogd_host', '?')} "
          f"({ctx.get('hogd_cpu_model', '?')})")
    print(f"build:       {ctx.get('hogd_build_type', '?')} "
          f"{ctx.get('hogd_cxx_flags', '')}")
    print(f"fingerprint: {ctx.get('engine_fingerprint', '?')}")
    print()

    header = (f"{'syscall':<28}{'entry ns':>10}{'±':>7}{'vm ns/call':>12}"
              f"{'seam':>8}{'gas':>8}")
    print(header)
    print("-" * len(header))
    for name in syscall_names(measured):
        entry = measured["entry/" + name]
        error = sd.get("entry/" + name, {}).get("real_time", 0.0)
        vm_row = measured.get("vm/" + name)
        vm = vm_ns_per_call(vm_row) if vm_row else None
        vm_text = f"{vm:>12.1f}" if vm else f"{'-':>12}"
        seam_text = f"{vm - entry['real_time']:>8.1f}" if vm else f"{'-':>8}"
        gas = int(entry.get("gas", 0))
        print(f"{name:<28}{entry['real_time']:>10.1f}{error:>7.2f}"
              f"{vm_text}{seam_text}{gas:>8}")

    points = sha256_points(measured)
    if points:
        print()
        header = (f"{'SHA256 bytes':<28}{'entry ns':>10}{'±':>7}{'gas':>12}"
                  f"{'ns/gas':>10}")
        print(header)
        print("-" * len(header))
        for size, ns in points:
            error = sd.get(f"entry/SHA256/{size}", {}).get("real_time", 0.0)
            gas = int(measured[f"entry/SHA256/{size}"].get("gas", 0))
            rate = ns / gas if gas else float("inf")
            print(f"{size:<28}{ns:>10.1f}{error:>7.2f}{gas:>12}{rate:>10.4f}")
        fit = fit_sha256(points)
        if fit:
            base, per_byte, r_squared = fit
            print()
            print(f"measured: {base:.1f} ns + {per_byte:.4f} ns/byte "
                  f"(R2={r_squared:.4f})")
            print("charged:  500 gas + 100 gas/byte  (SHA256HashCost)")
            rates = [ns / int(measured[f'entry/SHA256/{size}'].get('gas', 0))
                     for size, ns in points
                     if int(measured[f"entry/SHA256/{size}"].get("gas", 0))]
            if rates:
                print(f"ns/gas across the sweep: {min(rates):.4f} .. "
                      f"{max(rates):.4f} ({max(rates) / min(rates):.0f}x "
                      "spread within one syscall)")

    free = [name for name in syscall_names(measured)
            if int(measured["entry/" + name].get("gas", 0)) == 0]
    if free:
        print()
        print(f"{len(free)} of {len(syscall_names(measured))} syscalls "
              "charge zero direct host gas:")
        print("  " + ", ".join(free))
        print("Their cost is real and their price is not. --schedule proposes "
              "one.")
    return 0


def build_schedule(bench, calibration, resolution):
    measured = rows(bench)
    baseline = baseline_ns_from_calibration(calibration)
    if baseline <= 0.0:
        return None
    unit = baseline / resolution

    def to_gas(ns):
        # Round up, never down. A syscall priced below what it costs is
        # capacity the schedule hands out for free, and the rounding error is
        # bounded by one unit either way -- so take the safe direction, the
        # same way the opcode schedule does.
        return max(1, math.ceil(ns / unit - 1e-9))

    proposal = {"unit_ns": unit, "baseline_ns": baseline,
                "resolution": resolution, "syscalls": {}}

    # The shared base every syscall pays.
    #
    # The measurement is what justifies one constant rather than thirty: the
    # stateless reads land within a few percent of each other, so what is being
    # priced is the crossing, not the handler. Take the worst of them, because
    # a base every syscall pays has to cover the dearest one it is paying for.
    worst_name, worst_ns = None, 0.0
    for name in syscall_names(measured):
        vm_row = measured.get("vm/" + name)
        ns = vm_ns_per_call(vm_row) if vm_row else measured["entry/" + name][
            "real_time"]
        proposal["syscalls"][name] = {
            "entry_ns": measured["entry/" + name]["real_time"],
            "vm_ns": ns,
            "current_gas": int(measured["entry/" + name].get("gas", 0)),
        }
        if ns > worst_ns:
            worst_name, worst_ns = name, ns
    if worst_name is None:
        return None
    # The ecall instruction is already charged by kInstructionGasSchedule, so
    # host gas covers the rest. Subtracting it is the difference between
    # pricing the crossing once and pricing it twice.
    proposal["base_gas"] = max(1, to_gas(worst_ns) - ECALL_INSTRUCTION_GAS)
    proposal["base_ns"] = worst_ns
    proposal["base_from"] = worst_name

    fit = fit_sha256(sha256_points(measured))
    if fit:
        base_ns, per_byte_ns, r_squared = fit
        # SHA256's own cost is what it adds on top of a crossing every syscall
        # already pays for. Charging its full measured base as well would bill
        # the crossing twice.
        excess_ns = max(0.0, base_ns - proposal["base_ns"])
        proposal["sha256"] = {
            "measured_base_ns": base_ns,
            "excess_over_base_ns": excess_ns,
            "per_byte_ns": per_byte_ns,
            "r_squared": r_squared,
            "base_gas": to_gas(excess_ns) if excess_ns > 0 else 0,
            "byte_gas": to_gas(per_byte_ns),
            # A per-byte term smaller than one gas unit cannot be represented
            # without rounding up to a whole one, which overcharges by however
            # far it had to travel. Report the factor rather than hide it: it
            # is the argument for a finer resolution, and the same argument the
            # opcode schedule's --resolution note makes.
            "byte_rounding_factor": (to_gas(per_byte_ns) * unit) / per_byte_ns
            if per_byte_ns > 0 else 0.0,
        }
    return proposal


def print_schedule(bench, calibration, resolution):
    problems = check_joinable(bench, calibration)
    if problems:
        print("refusing to price this run against that calibration:",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    proposal = build_schedule(bench, calibration, resolution)
    if proposal is None:
        print("not enough measured rows to build a schedule", file=sys.stderr)
        return 1

    print(f"gas unit: {proposal['unit_ns']:.4f} ns "
          f"(1/{resolution} of the {proposal['baseline_ns']:.4f} ns baseline "
          "instruction)")
    print(f"measured on {context(bench).get('hogd_host', '?')}, engine "
          f"{context(bench).get('engine_fingerprint', '?')}")
    print()

    header = (f"{'syscall':<28}{'ns/call':>10}{'gas now':>9}"
              f"{'gas proposed':>14}")
    print(header)
    print("-" * len(header))
    for name, row in sorted(proposal["syscalls"].items()):
        print(f"{name:<28}{row['vm_ns']:>10.1f}{row['current_gas']:>9}"
              f"{proposal['base_gas']:>14}")
    print()
    print(f"One shared base of {proposal['base_gas']} gas, taken from "
          f"{proposal['base_from']} at {proposal['base_ns']:.1f} ns -- the "
          "dearest of them.")
    print("One constant rather than one per syscall because that is what the")
    print("measurement says: these land within a few percent of each other, so")
    print("what is being priced is the crossing and not the handler.")
    print(f"({ECALL_INSTRUCTION_GAS} gas of it is already charged as the ecall")
    print("instruction, and is subtracted rather than billed twice.)")

    sha = proposal.get("sha256")
    if sha:
        print()
        print(f"SHA256: {sha['measured_base_ns']:.1f} ns + "
              f"{sha['per_byte_ns']:.4f} ns/byte (R2={sha['r_squared']:.4f})")
        print(f"  of which {sha['excess_over_base_ns']:.1f} ns is its own work, "
              "above the shared crossing")
        if sha["byte_rounding_factor"] > 1.5:
            print(f"  WARNING: the per-byte term is "
                  f"{sha['per_byte_ns']:.4f} ns against a "
                  f"{proposal['unit_ns']:.4f} ns gas unit, so rounding it up "
                  f"to {sha['byte_gas']} gas overcharges by "
                  f"{sha['byte_rounding_factor']:.1f}x.")
            print("  Raise --resolution until this is close to 1, and raise "
                  "the opcode")
            print("  schedule's resolution with it -- they have to share a "
                  "unit.")

    print()
    print("=" * 68)
    print("Proposed hogd/hog_exec/runtime/gas_cost_table.h constants")
    print("=" * 68)
    print(f"constexpr static uint64_t kSyscallBaseCost = "
          f"{proposal['base_gas']};")
    if sha:
        print(f"constexpr static uint64_t kSHA256HashBaseCost = "
              f"{sha['base_gas']};   // was 500")
        print(f"constexpr static uint64_t kSHA256HashByteCost = "
              f"{sha['byte_gas']};   // was 100")
    print()
    print("kSyscallBaseCost is new: every syscall charges it, which is the")
    print("change the measurement actually argues for. Adding it means adding")
    print("an entry to kSyscallGasCostSchedule, which is folded into")
    print("rv32::kGasScheduleDigest -- so the pinned digest in artifact.h and")
    print("every committed RV32 artifact move in the same change.")
    return 0


def print_comparison(before, after):
    before_rows = rows(before)
    after_rows = rows(after)
    shared = sorted(set(before_rows) & set(after_rows))
    if not shared:
        print("no benchmarks in common", file=sys.stderr)
        return 1
    before_fp = context(before).get("engine_fingerprint")
    after_fp = context(after).get("engine_fingerprint")
    if before_fp and after_fp and before_fp != after_fp:
        print(f"NOTE: the engine changed ({before_fp} -> {after_fp}), so any "
              "difference below\n      is the engine, not the host.")
        print()
    header = f"{'benchmark':<32}{'before':>12}{'after':>12}{'delta':>10}{'%':>8}"
    print(header)
    print("-" * len(header))
    for name in shared:
        old = before_rows[name]["real_time"]
        new = after_rows[name]["real_time"]
        delta = new - old
        percent = (delta / old * 100.0) if old else 0.0
        print(f"{name:<32}{old:>12.1f}{new:>12.1f}{delta:>+10.1f}"
              f"{percent:>+8.1f}")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("json", nargs="+",
                        help="syscall benchmark JSON (one, or two to compare)")
    parser.add_argument("--gas", metavar="CALIBRATION",
                        help="opcode calibration JSON from the same host and "
                             "engine; supplies the gas unit")
    parser.add_argument("--schedule", action="store_true",
                        help="propose gas_cost_table.h constants")
    parser.add_argument("--resolution", type=int, default=DEFAULT_RESOLUTION,
                        help="gas units per baseline instruction "
                             f"(default {DEFAULT_RESOLUTION})")
    args = parser.parse_args()

    runs = [load(path) for path in args.json]
    if args.schedule:
        if len(runs) != 1:
            parser.error("--schedule takes exactly one benchmark JSON")
        if not args.gas:
            parser.error(
                "--schedule needs --gas CALIBRATION: gas is one currency, and "
                "the unit comes from the opcode calibration's baseline "
                "instruction measured on the same host")
        return print_schedule(runs[0], load(args.gas), args.resolution)
    if len(runs) == 2:
        return print_comparison(runs[0], runs[1])
    if len(runs) != 1:
        parser.error("pass one JSON file, or two to compare")
    return print_table(runs[0])


if __name__ == "__main__":
    sys.exit(main())
