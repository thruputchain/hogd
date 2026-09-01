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

# A syscall counts as crossing-dominated -- and so contributes to the one
# shared base cost -- when it costs no more than this multiple of the cheapest
# syscall measured.
#
# The shared base rests on an argument that only holds for such syscalls: they
# land within a few percent of each other because what is being priced is the
# crossing and not the handler. A syscall that does real work breaks that
# argument, and taking a max over the lot silently prices every syscall at the
# dearest one -- which is how a bench-pool run proposed 17646 gas for every
# syscall in the table, that being what an ed25519 verification costs.
#
# 1.5x rather than something tighter: the reads spread about 11% on hog10 and
# more on a noisy host, and a syscall doing so little extra work that it hides
# inside that spread is one the shared base can honestly cover.
CROSSING_DOMINATED_FACTOR = 1.5

# Denominators a per-unit charge may use: cost = ceil(n * numerator /
# denominator). Powers of two so the division is exact and cheap, up to
# SHA256's own 64-byte block -- past which the charge stops tracking input size
# at a granularity a guest can feel.
#
# This exists because a per-byte cost can land well under one gas unit, and an
# integer gas-per-byte then has no way to express it: rounding 0.46 gas up to 1
# overcharges by 2.2x, which is a worse mispricing than most of what the opcode
# calibration set out to fix. Splitting the charge over several bytes gets the
# precision without touching the gas unit -- and touching the gas unit means
# rescaling the opcode schedule and the block limits with it, since the two
# cannot be denominated differently.
RATE_DENOMINATORS = (1, 2, 4, 8, 16, 32, 64)

# Stop refining once the rounding overcharge is inside this, and take the
# simplest denominator that gets there rather than the most accurate one
# available.
#
# The threshold is set by the measurement's own reproducibility, not by how
# small it can be made. Two runs of this benchmark half an hour apart on a
# shared host moved individual syscalls by up to 11%; a denominator chosen to
# bring a rounding error from 9% down to 3% is fitting a digit the underlying
# number does not have, and pays for it in a consensus constant that nobody can
# read. 1/2 gas per byte is a thing a contract author can hold in their head.
# 15/32 is not, and it is not more correct -- it is more precise about a
# quantity known less precisely than that.
RATE_OVERCHARGE_TARGET = 1.10

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

    Rows the benchmark failed are dropped rather than reported. A skipped row
    still appears in the JSON, with a real name and a real_time of zero, and a
    reader -- or worse, the schedule builder -- has no way to tell that zero
    from a measurement. One bench-pool run proposed a shared syscall base of
    17646 gas because two failed VerifySignature rows sat in the table looking
    like data.
    """
    medians = {}
    plain = {}
    for entry in data.get("benchmarks", []):
        name = entry.get("run_name") or entry.get("name")
        if not name or entry.get("error_occurred"):
            continue
        if entry.get("aggregate_name") == "median":
            medians[name] = entry
        elif "aggregate_name" not in entry:
            plain[name] = entry
    return plain | medians


def failed_rows(data):
    """Rows the benchmark skipped, with the reason it gave.

    Reported separately and prominently: a syscall that could not be measured
    is the most important line in the file, because everything else is complete
    enough to look finished without it.
    """
    seen = {}
    for entry in data.get("benchmarks", []):
        if not entry.get("error_occurred"):
            continue
        name = entry.get("run_name") or entry.get("name")
        if name:
            seen.setdefault(name, entry.get("error_message", ""))
    return seen


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


def choose_rate(gas_per_unit):
    """Express a sub-unit rate as ceil(n * numerator / denominator).

    Returns (numerator, denominator, overcharge). The smallest denominator that
    brings the overcharge inside RATE_OVERCHARGE_TARGET wins, so the simplest
    expression that is accurate enough is the one proposed rather than the most
    accurate one available.
    """
    if gas_per_unit <= 0.0:
        return 1, 1, 1.0
    best = None
    for denominator in RATE_DENOMINATORS:
        numerator = max(1, math.ceil(gas_per_unit * denominator - 1e-9))
        overcharge = (numerator / denominator) / gas_per_unit
        if best is None or overcharge < best[2]:
            best = (numerator, denominator, overcharge)
        if overcharge <= RATE_OVERCHARGE_TARGET:
            return numerator, denominator, overcharge
    return best


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

    # Absence is refused, not waved through. A file that cannot say which
    # engine or machine it came from -- truncated, hand-edited, or written by
    # an older binary -- cannot be shown to be joinable, and "we could not
    # check" is not the same as "it is fine". Treating a missing field as
    # compatible is what would let this function's own stated invariant be
    # skipped by exactly the inputs least worth trusting.
    bench_fp = bench_ctx.get("engine_fingerprint")
    cal_fp = cal_prov.get("engine_fingerprint")
    if not bench_fp or not cal_fp:
        problems.append(
            "missing engine fingerprint: "
            f"{'benchmark' if not bench_fp else 'calibration'} does not record "
            "one, so there is no way to tell whether these describe the same "
            "engine. Re-run it with a binary that stamps its provenance.")
    elif bench_fp != cal_fp:
        problems.append(
            f"engine fingerprint {bench_fp} (benchmark) != {cal_fp} "
            "(calibration): these measured different engines, so the "
            "calibration's baseline does not describe the benchmark's host "
            "instruction.")

    bench_host = bench_ctx.get("hogd_host")
    cal_host = cal_prov.get("host")
    if not bench_host or not cal_host:
        problems.append(
            "missing host: "
            f"{'benchmark' if not bench_host else 'calibration'} does not "
            "record one, so there is no way to tell whether the nanoseconds "
            "and the gas unit come from the same machine.")
    elif bench_host != cal_host:
        problems.append(
            f"host {bench_host} (benchmark) != {cal_host} (calibration): "
            "nanoseconds are a fact about one machine, and the gas unit would "
            "come from the other one.")

    if bench_ctx.get("hogd_production_build") == "false":
        problems.append("the benchmark was not a Release build")
    if cal_prov.get("production_build") is False:
        problems.append("the calibration was not a Release build")
    return problems


# --- independent cross-check against `openssl speed` -------------------------
#
# What each comparison can establish differs, so each carries its own tolerance
# rather than one shared "close enough".
#
# SHA256 is the same code on both sides: hogd/crypto/crypto.h calls
# SHA256_Init/Update/Final, so the syscall and openssl run one libcrypto's
# compression function. The per-byte terms should agree closely, and a real gap
# means the benchmark is timing something other than hashing.
#
# ed25519 is not: hogd verifies with curve25519-dalek and OpenSSL 3.0 has its
# own implementation, several times slower than dalek's vectorised backends, so
# a factor apart is expected and the tolerance has to be loose enough to allow
# it. That makes this the weaker of the two checks, and it is worth being clear
# about what it can still catch: a row that is not doing the work at all --
# hoisted out of the timing loop, measuring a no-op, wired to the wrong syscall
# -- lands orders of magnitude out, not a factor. It is a backstop, not a
# validation of the implementation. A row that takes ed25519's cheap rejection
# path is caught earlier and far more sharply: verification fails, the syscall
# returns an error, and the benchmark skips the row rather than timing it.
SHA256_CROSSCHECK_TOLERANCE = 0.25
ED25519_CROSSCHECK_FACTOR = 10.0


def parse_openssl_speed(data):
    """Rates out of `openssl speed -mr` output.

    Returns {"sha256": {block_size: bytes_per_second}, "ed25519": {...}}.

    Reads the machine-readable +H/+F lines rather than the human table, which
    has been relaid out more than once across OpenSSL releases.
    """
    sizes = []
    sha256 = {}
    ed25519 = {}
    for line in data.get("raw", []):
        fields = line.split(":")
        if fields[0] == "+H":
            sizes = [int(f) for f in fields[1:] if f.strip().isdigit()]
        elif fields[0] == "+F" and "sha256" in fields:
            tail = fields[fields.index("sha256") + 1:]
            rates = [float(f) for f in tail if f]
            sha256 = dict(zip(sizes, rates))
        elif fields[0] == "+F6" and "Ed25519" in fields:
            rest = fields[fields.index("Ed25519") + 1:]
            if len(rest) >= 2:
                ed25519 = {"sign_per_second": float(rest[0]),
                           "verify_per_second": float(rest[1])}
    return {"sha256": sha256, "ed25519": ed25519}


def print_crosscheck(data, openssl):
    measured = rows(data)
    speed = parse_openssl_speed(openssl)
    print()
    # Same rule --gas already enforces: a cross-check across two machines is
    # not a weaker check, it is a misleading one. Both numbers stay plausible
    # and their ratio means nothing. This is not hypothetical -- a container
    # migrated silicon mid-session and gained SHA-NI, and the resulting
    # fresh-openssl-vs-stale-benchmark pair read as a 4x fault in hogd.
    theirs = openssl.get("cpu_model")
    ours = context(data).get("hogd_cpu_model")
    if theirs and ours and theirs != ours:
        print("no cross-check: these files are from different machines")
        print(f"  benchmark: {ours}")
        print(f"  openssl:   {theirs}")
        print("  Re-run scripts/rv32_openssl_speed.sh on the host the "
              "benchmark came from.")
        return 0
    print("cross-check against openssl speed "
          f"({openssl.get('openssl_version', 'unknown version')})")
    # The architecture flags only, and only when there are some. They are what
    # explains the ed25519 row: a libcrypto built -march=x86-64 runs Ed25519 as
    # generic C, while SHA256 is unaffected because its SHA-NI assembly is
    # dispatched at runtime. The rest of a compiler line explains nothing, so a
    # truncated one is noise -- the full string stays in the JSON either way.
    march = [flag for flag in openssl.get("openssl_compiler", "").split()
             if flag.startswith(("-march", "-mtune"))]
    if march:
        print(f"openssl built for: {' '.join(march)}")
    print("-" * 76)

    lines = []
    sha_rates = speed["sha256"]
    fit = fit_sha256(sha256_points(measured))
    if sha_rates and fit:
        # openssl's own per-call overhead is amortised away at its largest
        # block, which is where its number is closest to pure compression --
        # the same thing hogd's *slope* isolates by construction.
        block = max(sha_rates)
        openssl_ns = 1e9 / sha_rates[block]
        ours_ns = fit[1]
        ratio = ours_ns / openssl_ns
        ok = abs(ratio - 1.0) <= SHA256_CROSSCHECK_TOLERANCE
        lines.append(("SHA256 ns/byte", f"{openssl_ns:.4f}", f"{ours_ns:.4f}",
                      ratio, ok,
                      f"openssl at {block}-byte blocks vs hogd's sweep slope; "
                      "same libcrypto, so these should agree"))

    verify = speed["ed25519"].get("verify_per_second")
    ours = [row for name, row in measured.items()
            if name.startswith("entry/VerifySignature/")]
    if verify and ours:
        # The cheapest row: the shortest message is the purest constant, and
        # openssl times a short fixed message too.
        ours_ns = min(row["real_time"] for row in ours)
        openssl_ns = 1e9 / verify
        ratio = ours_ns / openssl_ns
        low = 1.0 / ED25519_CROSSCHECK_FACTOR
        ok = low <= ratio <= ED25519_CROSSCHECK_FACTOR
        lines.append(("ed25519 verify ns", f"{openssl_ns:.0f}",
                      f"{ours_ns:.0f}",
                      ratio, ok,
                      "OpenSSL's Ed25519 is generic C with no vectorised "
                      "backend, and a distro libcrypto is built for baseline "
                      "x86-64; dalek gets target-cpu=native. A factor is "
                      "expected -- an order of magnitude is the test"))

    if not lines:
        print("  nothing comparable in this pair of files")
        return 0

    print(f"  {'primitive':<20}{'openssl':>12}{'hogd':>12}"
          f"{'ratio':>9}  verdict")
    all_ok = True
    for label, theirs, ours_text, ratio, ok, note in lines:
        print(f"  {label:<20}{theirs:>12}{ours_text:>12}{ratio:>8.2f}x  "
              f"{'ok' if ok else 'INVESTIGATE'}")
        print(f"    {note}")
        all_ok = all_ok and ok
    if not all_ok:
        print()
        print("  A ratio outside tolerance does not by itself mean the gas")
        print("  numbers are wrong -- but it does mean this run cannot vouch")
        print("  for them, and the row should be explained before adopting.")
    # Deliberately still success. This is a sanity check on a measurement, not
    # a gate on one: an OpenSSL upgrade on the bench host can move these ratios
    # without anything being wrong with hogd, and failing the calibration job
    # over that would cost the opcode numbers running beside it. The word
    # INVESTIGATE in the log is the signal.
    return 0


# The ecall crossing is a fixed cost of order 10ns, so subtracting entry from
# vm only recovers it while both are measured far more precisely than their
# difference. For a syscall that does real work -- an ed25519 verify, a 4KiB
# hash -- the two are large, noisy and nearly equal, and the subtraction is
# dominated by run-to-run spread: it produced a 5.7us crossing and a *negative*
# one in the same table. Report the number only when the run's own stddevs say
# it is resolvable, and otherwise say so rather than printing noise.
SEAM_SIGMA = 3.0


def seam_text(name, entry_ns, entry_sd, vm_ns, vm_row, sd):
    difference = vm_ns - entry_ns
    vm_sd = sd.get("vm/" + name, {}).get("real_time", 0.0)
    # vm rows time a whole batch of calls, so their stddev is in batch units.
    # Scale it per call, exactly as vm_ns_per_call scales the mean.
    calls = calls_per_vm_run(vm_row)
    if calls:
        vm_sd /= calls
    noise = SEAM_SIGMA * math.hypot(entry_sd, vm_sd)
    # A run with no repetitions carries no stddevs at all; there is nothing to
    # test against, so show the difference and let the reader judge.
    if noise == 0.0 or abs(difference) > noise:
        return f"{difference:.1f}"
    return "~0"


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

    header = (f"{'syscall':<28}{'entry ns':>11}{'±':>9}{'vm ns/call':>12}"
              f"{'seam':>8}{'gas':>8}")
    print(header)
    print("-" * len(header))
    for name in syscall_names(measured):
        entry = measured["entry/" + name]
        error = sd.get("entry/" + name, {}).get("real_time", 0.0)
        vm_row = measured.get("vm/" + name)
        vm = vm_ns_per_call(vm_row) if vm_row else None
        vm_text = f"{vm:>12.1f}" if vm else f"{'-':>12}"
        crossing = seam_text(name, entry["real_time"], error, vm, vm_row, sd) \
            if vm else "-"
        gas = int(entry.get("gas", 0))
        print(f"{name:<28}{entry['real_time']:>11.1f}{error:>9.2f}"
              f"{vm_text}{crossing:>8}{gas:>8}")

    points = sha256_points(measured)
    if points:
        print()
        header = (f"{'SHA256 bytes':<28}{'entry ns':>11}{'±':>9}{'gas':>12}"
                  f"{'ns/gas':>10}")
        print(header)
        print("-" * len(header))
        for size, ns in points:
            error = sd.get(f"entry/SHA256/{size}", {}).get("real_time", 0.0)
            gas = int(measured[f"entry/SHA256/{size}"].get("gas", 0))
            rate = ns / gas if gas else float("inf")
            print(f"{size:<28}{ns:>11.1f}{error:>9.2f}{gas:>12}{rate:>10.4f}")
        fit = fit_sha256(points)
        if fit:
            base, per_byte, r_squared = fit
            print()
            print(f"measured: {base:.1f} ns + {per_byte:.4f} ns/byte "
                  f"(R2={r_squared:.4f})")
            print(f"charged:  {COMMITTED_SYSCALL_BASE_GAS} base + "
                  f"{COMMITTED_SHA256_BASE_GAS} gas + "
                  f"{COMMITTED_SHA256_BLOCK_GAS} gas per "
                  f"{SHA256_BLOCK_BYTES}-byte block  (SHA256HashCost)")
            rates = [ns / int(measured[f'entry/SHA256/{size}'].get('gas', 0))
                     for size, ns in points
                     if int(measured[f"entry/SHA256/{size}"].get("gas", 0))]
            if rates:
                print(f"ns/gas across the sweep: {min(rates):.4f} .. "
                      f"{max(rates):.4f} ({max(rates) / min(rates):.0f}x "
                      "spread within one syscall)")

    skipped = failed_rows(data)
    if skipped:
        print()
        print(f"{len(skipped)} row(s) FAILED and are excluded from every "
              "number above:")
        for name, message in sorted(skipped.items()):
            print(f"  {name}: {message}")

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


# SHA-256 geometry, and the schedule currently committed in
# hogd/hog_exec/runtime/gas_cost_table.h. Mirrored here so the report can say
# what a guest is billed today; they must move together with that header, which
# is why they are named once rather than spelled inline at each use.
SHA256_BLOCK_BYTES = 64
SHA256_PAD_BYTES = 9          # 0x80 plus an 8-byte big-endian bit length
COMMITTED_SYSCALL_BASE_GAS = 50
COMMITTED_SHA256_BASE_GAS = 2
COMMITTED_SHA256_BLOCK_GAS = 22


def sha256_blocks(byte_len):
    """Compression blocks a SHA-256 of this length performs, padding included."""
    return (byte_len + SHA256_PAD_BYTES + SHA256_BLOCK_BYTES - 1) \
        // SHA256_BLOCK_BYTES


def committed_sha256_gas(byte_len):
    return (COMMITTED_SYSCALL_BASE_GAS + COMMITTED_SHA256_BASE_GAS
            + COMMITTED_SHA256_BLOCK_GAS * sha256_blocks(byte_len))


def fit_sha256_blocks(points):
    """Least squares against compression blocks: (base_ns, ns_per_block, r2).

    The block is what SHA-256 actually does, and the measurement says so --
    this fit lands at R2 = 1.000000 where the per-byte one merely rounds to it.
    Fitting the real unit is also what removes the need for a rate denominator:
    a per-byte price is a fraction no integer can express, and the divisor that
    would fix it is exactly the sort of billing-relevant constant that is easy
    to leave out of the gas-schedule digest.
    """
    if len(points) < MIN_SWEEP_POINTS:
        return None
    xs = [sha256_blocks(n) for n, _ in points]
    ys = [ns for _, ns in points]
    m = len(xs)
    sx, sy = sum(xs), sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = m * sxx - sx * sx
    if denom == 0:
        return None
    slope = (m * sxy - sx * sy) / denom
    intercept = (sy - slope * sx) / m
    mean = sy / m
    ss_tot = sum((y - mean) ** 2 for y in ys)
    ss_res = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    r_squared = 1.0 if ss_tot == 0 else 1.0 - ss_res / ss_tot
    return intercept, slope, r_squared


def build_schedule(bench, calibration, resolution, margin=0.0):
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
        return max(1, math.ceil(ns * (1.0 + margin) / unit - 1e-9))

    proposal = {"unit_ns": unit, "baseline_ns": baseline,
                "resolution": resolution, "margin": margin, "syscalls": {}}

    # Every measured syscall, with what a guest actually causes: the vm row
    # where there is one -- crossing included -- and the entry row otherwise.
    for name in syscall_names(measured):
        vm_row = measured.get("vm/" + name)
        ns = vm_ns_per_call(vm_row) if vm_row else measured["entry/" + name][
            "real_time"]
        proposal["syscalls"][name] = {
            "entry_ns": measured["entry/" + name]["real_time"],
            "vm_ns": ns,
            "current_gas": int(measured["entry/" + name].get("gas", 0)),
        }
    if not proposal["syscalls"]:
        return None

    # Split them: the ones whose cost is the crossing, and the ones that do
    # work of their own. Only the first group can be covered by a single shared
    # constant, and only the second needs a cost of its own -- so the split is
    # the schedule's shape, not a presentational choice.
    cheapest = min(row["vm_ns"] for row in proposal["syscalls"].values())
    crossing = {name: row for name, row in proposal["syscalls"].items()
                if row["vm_ns"] <= cheapest * CROSSING_DOMINATED_FACTOR}
    heavy = {name: row for name, row in proposal["syscalls"].items()
             if name not in crossing}
    proposal["crossing"] = sorted(crossing)
    # Gas for the heavy rows is computed here, with to_gas, rather than at the
    # point of printing. Doing it in the printer is how these two numbers came
    # to ignore --margin entirely: to_gas is where the margin is applied, and a
    # second, inline `ceil(ns / unit)` beside it looks equivalent and is not.
    # The run then prints a "5% margin" proposal whose most expensive entries
    # carry no margin at all, which is the one place it would matter most.
    crossing_worst_ns = max(r["vm_ns"] for r in crossing.values())
    proposal["heavy"] = {
        name: {**row,
               "own_ns": max(0.0, row["vm_ns"] - crossing_worst_ns),
               # base + its own work, rounded up once on the total rather
               # than twice.
               "total_gas": to_gas(row["vm_ns"]) - ECALL_INSTRUCTION_GAS,
               "own_gas": to_gas(max(0.0, row["vm_ns"] - crossing_worst_ns))}
        for name, row in sorted(heavy.items())}

    # Worst case within the crossing-dominated group: a base every syscall pays
    # has to cover the dearest one it is paying for.
    worst_name = max(crossing, key=lambda n: crossing[n]["vm_ns"])
    worst_ns = crossing[worst_name]["vm_ns"]
    # The ecall instruction is already charged by kInstructionGasSchedule, so
    # host gas covers the rest. Subtracting it is the difference between
    # pricing the crossing once and pricing it twice.
    # The crossing, as this run measured it: what a vm row costs beyond its
    # entry row. Needed to price the SHA256 sweep, whose rows are entry-only.
    seams = []
    for name in syscall_names(measured):
        vm_row = measured.get("vm/" + name)
        if not vm_row:
            continue
        vm_ns = vm_ns_per_call(vm_row)
        if vm_ns:
            seams.append(vm_ns - measured["entry/" + name]["real_time"])
    proposal["seam_ns"] = statistics.median(seams) if seams else 0.0
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
        gas_per_byte = per_byte_ns * (1.0 + margin) / unit
        flat_gas = to_gas(per_byte_ns)
        numerator, denominator, overcharge = choose_rate(gas_per_byte)
        proposal["sha256"] = {
            "measured_base_ns": base_ns,
            "excess_over_base_ns": excess_ns,
            "per_byte_ns": per_byte_ns,
            "gas_per_byte": gas_per_byte,
            "r_squared": r_squared,
            "base_gas": to_gas(excess_ns) if excess_ns > 0 else 0,
            # What a flat integer per-byte cost would have to be, and how far
            # it overshoots. Kept beside the proposal because the gap between
            # the two is the entire argument for the divisor.
            "flat_byte_gas": flat_gas,
            "flat_overcharge": (flat_gas * unit) / per_byte_ns
            if per_byte_ns > 0 else 0.0,
            "byte_numerator": numerator,
            "byte_denominator": denominator,
            "byte_overcharge": overcharge,
        }
        block_fit = fit_sha256_blocks(sha256_points(measured))
        if block_fit:
            block_base_ns, per_block_ns, block_r2 = block_fit
            block_excess_ns = max(0.0, block_base_ns - proposal["base_ns"])
            proposal["sha256"].update({
                "block_base_ns": block_base_ns,
                "per_block_ns": per_block_ns,
                "block_r_squared": block_r2,
                "block_gas": to_gas(per_block_ns),
                "block_base_gas": (to_gas(block_excess_ns)
                                   if block_excess_ns > 0 else 0),
            })
        proposal["sweep"] = sha256_points(measured)
    return proposal


def print_schedule(bench, calibration, resolution, margin=0.0):
    problems = check_joinable(bench, calibration)
    if problems:
        print("refusing to price this run against that calibration:",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    skipped = failed_rows(bench)
    if skipped:
        print(f"WARNING: {len(skipped)} row(s) failed and are excluded:")
        for name, message in sorted(skipped.items()):
            print(f"  {name}: {message}")
        print()
    proposal = build_schedule(bench, calibration, resolution, margin)
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
    for name in proposal["crossing"]:
        row = proposal["syscalls"][name]
        print(f"{name:<28}{row['vm_ns']:>10.1f}{row['current_gas']:>9}"
              f"{proposal['base_gas']:>14}")
    unit = proposal["unit_ns"]
    for name, row in proposal["heavy"].items():
        total = row["total_gas"]
        print(f"{name:<28}{row['vm_ns']:>10.1f}{row['current_gas']:>9}"
              f"{total:>14}  = base + {total - proposal['base_gas']}")
    print()
    # How much room the proposal leaves over the dearest thing it has to
    # cover. Worth printing because rounding up to the next whole gas is the
    # only headroom a zero-margin proposal has, and at this unit that is a few
    # percent -- narrower than the run-to-run spread of the measurement it came
    # from. Whether that is enough is a policy call, which is what --margin is
    # for; the report's job is to make the size of it visible rather than let
    # it be discovered later.
    budget_ns = ((proposal["base_gas"] + ECALL_INSTRUCTION_GAS) *
                 proposal["unit_ns"])
    headroom = budget_ns / proposal["base_ns"] - 1.0 if proposal["base_ns"] else 0.0
    print(f"One shared base of {proposal['base_gas']} gas, taken from "
          f"{proposal['base_from']} at {proposal['base_ns']:.1f} ns -- the "
          "dearest of them.")
    if margin:
        print(f"Includes a {margin * 100:.0f}% margin, applied before "
              "rounding.")
    print(f"That buys {budget_ns:.1f} ns of budget: {headroom * 100:+.1f}% "
          "over the dearest measured call.")
    print("One constant rather than one per syscall because that is what the")
    print("measurement says: these land within a few percent of each other, so")
    print("what is being priced is the crossing and not the handler.")
    if proposal["heavy"]:
        print()
        print(f"{len(proposal['heavy'])} syscall(s) cost more than "
              f"{CROSSING_DOMINATED_FACTOR:g}x the cheapest, so they do work "
              "the shared base")
        print("cannot cover and need a cost of their own on top of it:")
        for name, row in proposal["heavy"].items():
            print(f"  {name:<26}{row['own_ns']:>9.1f} ns of its own "
                  f"-> {row['own_gas']} gas above the base")
        print("A single shared constant taken over all of them would price "
              "every")
        print("syscall at the dearest, which is the bug this split exists to "
              "prevent.")
    print(f"({ECALL_INSTRUCTION_GAS} gas of it is already charged as the ecall")
    print("instruction, and is subtracted rather than billed twice.)")

    sha = proposal.get("sha256")
    if sha:
        print()
        print(f"SHA256: {sha['measured_base_ns']:.1f} ns + "
              f"{sha['per_byte_ns']:.4f} ns/byte (R2={sha['r_squared']:.4f})")
        print(f"  of which {sha['excess_over_base_ns']:.1f} ns is its own work, "
              "above the shared crossing")
        print(f"  per byte that is {sha['gas_per_byte']:.4f} gas, which no "
              "integer can express:")
        print(f"    flat {sha['flat_byte_gas']} gas/byte overcharges by "
              f"{sha['flat_overcharge']:.2f}x")
        print(f"    {sha['byte_numerator']} gas per "
              f"{sha['byte_denominator']} bytes overcharges by "
              f"{sha['byte_overcharge']:.2f}x")
        if sha["byte_denominator"] > 1:
            print("  The divisor is the cheap fix. The other one is a finer")
            print("  --resolution, which buys the same precision and rescales")
            print("  the opcode schedule and the block gas limits to get it.")

        # How even the pricing comes out, which is the question the constants
        # are answering and the one a list of constants cannot show. Same
        # metric the opcode schedule is judged by: a gas limit converts to a
        # wall-clock bound at the worst ns/gas, and everything above that is
        # capacity the schedule gives away.
        print()
        print("  Pricing quality across the size sweep (ns of real work per "
              "gas;")
        print(f"  a dispatched instruction is {proposal['unit_ns']:.4f}):")
        print(f"    {'bytes':>8}{'now':>10}{'flat':>10}{'proposed':>10}")
        rates = {"now": [], "flat": [], "proposed": []}
        for size, ns in proposal.get("sweep", []):
            # What the guest causes, not what the handler alone costs: the
            # crossing is part of the price being set.
            real = ns + proposal["seam_ns"]
            now = committed_sha256_gas(size)
            flat = (proposal["base_gas"] + sha["base_gas"] +
                    sha["flat_byte_gas"] * size)
            proposed = (proposal["base_gas"] + sha.get("block_base_gas", 0) +
                        sha.get("block_gas", 0) * sha256_blocks(size))
            for key, gas in (("now", now), ("flat", flat),
                             ("proposed", proposed)):
                rates[key].append(real / gas if gas else float("inf"))
            print(f"    {size:>8}{real / now:>10.4f}{real / flat:>10.4f}"
                  f"{real / proposed:>10.4f}")
        print(f"    {'spread':>8}", end="")
        for key in ("now", "flat", "proposed"):
            values = [r for r in rates[key] if r > 0]
            print(f"{max(values) / min(values):>9.2f}x", end="")
        print()
        print("  Spread is worst-over-best: how differently the same syscall "
              "is")
        print("  billed depending on an argument the caller picks. Lower is "
              "flatter.")

    print()
    print("=" * 68)
    print("Proposed hogd/hog_exec/runtime/gas_cost_table.h constants")
    print("=" * 68)
    print(f"constexpr static uint64_t kSyscallBaseCost = "
          f"{proposal['base_gas']};")
    if sha and "block_gas" in sha:
        print(f"constexpr static uint64_t kSHA256HashBaseCost = "
              f"{sha['block_base_gas']};"
              f"   // committed: {COMMITTED_SHA256_BASE_GAS}")
        print(f"constexpr static uint64_t kSHA256HashBlockCost = "
              f"{sha['block_gas']};"
              f"  // committed: {COMMITTED_SHA256_BLOCK_GAS}")
        print()
        print("  SHA256HashCost charges")
        print(f"    base + block_cost * ceil((bytes + {SHA256_PAD_BYTES})"
              f" / {SHA256_BLOCK_BYTES})")
        print("  Per compression block, because that is the unit SHA-256")
        print(f"  actually works in: the fit is R2="
              f"{sha.get('block_r_squared', 0):.6f} against blocks. It also")
        print("  avoids a per-byte rate, which is a fraction no integer can")
        print("  express -- and the divisor that would fix that is exactly the")
        print("  kind of billing-relevant constant that is easy to leave out")
        print("  of the digest. gas_schedule_digest() in artifact.h hashes")
        print("  kSyscallGasCostSchedule's names and costs, not the formula,")
        print("  so any constant the formula reads has to be an")
        print("  entry in kSyscallGasCostSchedule to be hashed.")
    print()
    print("Changing any of these is a coordinated gas-schedule update: each")
    print("is an entry in kSyscallGasCostSchedule, which is folded into")
    print("rv32::kGasScheduleDigest -- so the pinned digest in artifact.h, the")
    print("artifact format fingerprint derived from it, and any artifact")
    print("stamped with the current schedule all move in the same change.")
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
    parser.add_argument("--margin", type=float, default=0.0, metavar="PERCENT",
                        help="safety margin applied to every measured cost "
                             "before rounding, as a percentage. Zero-margin "
                             "costs are worst-case-measured but their only "
                             "headroom is the rounding, which at resolution 1 "
                             "is a few percent -- narrower than this "
                             "benchmark reproduces to on a shared host.")
    parser.add_argument("--crosscheck", metavar="OPENSSL_JSON",
                        help="openssl speed JSON from scripts/"
                             "rv32_openssl_speed.sh, run on the same host; "
                             "prints an independent check of the SHA256 and "
                             "ed25519 numbers")
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
        status = print_schedule(runs[0], load(args.gas), args.resolution,
                                args.margin / 100.0)
    elif len(runs) == 2:
        if args.crosscheck:
            parser.error("--crosscheck applies to a single run, not a "
                         "comparison of two")
        return print_comparison(runs[0], runs[1])
    elif len(runs) != 1:
        parser.error("pass one JSON file, or two to compare")
    else:
        status = print_table(runs[0])
    if args.crosscheck and status == 0:
        # Tolerate a missing or unreadable file rather than raising. The check
        # is optional by construction: the schedule runs the openssl step with
        # `|| true`, so a host without openssl -- or an openssl whose `speed`
        # rejects one of the algorithms -- reaches here with nothing to read.
        # Failing on that would turn a skipped sanity check into a traceback
        # and a non-zero exit on a run whose numbers are perfectly good.
        try:
            crosscheck = load(args.crosscheck)
        except (OSError, json.JSONDecodeError) as error:
            print(f"\nno cross-check: {error}", file=sys.stderr)
        else:
            status = print_crosscheck(runs[0], crosscheck)
    return status


if __name__ == "__main__":
    sys.exit(main())
