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
import textwrap

from rv32_gas_units import DEFAULT_RESOLUTION, baseline_ns, gas_unit_ns

# One gas is 1/RESOLUTION of a baseline instruction. Matches
# rv32_gas_report.py's default; a syscall schedule built at a different
# resolution than the opcode schedule it sits beside is not a schedule.


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
# The bytestring family, swept on value length. Kept out of the syscall table
# for the reason the SHA256 rows are: a length sweep is one syscall measured
# many times, and listing every point beside the one-shot rows would both bury
# them and pull the gating baseline -- which is the median of the cheapest
# crossing rows -- toward whichever end of the sweep happened to be cheap.
BYTESTRING_ROW = re.compile(
    r"^(entry|vm)/(BytestringSet|BytestringGet|BytestringGetLen)/(\d+)$")
BYTESTRING_FAMILIES = ("BytestringSet", "BytestringGet", "BytestringGetLen")

# ML-DSA-65 VerifySignature, swept on message length. Kept out of the syscall
# table and out of every shared number -- the crossing group, the seam median
# that prices the SHA256 sweep, the guard baseline -- for two reasons. It is a
# length sweep, like SHA256 and the bytestrings. And its price is its own: the
# syscall charges it GasCost::verify_signature_mldsa65(len), which reads only
# kVerifySignatureMlDsa65BaseCost and kVerifySignatureMlDsa65BlockCost. The
# rows are reported as their own family, beside the ed25519 row of the same
# length, with an estimate of those two constants.
MLDSA_VERIFY_ROW = re.compile(r"^(entry|vm)/VerifySignatureMlDsa65/(\d+)$")
MLDSA_VERIFY_FAMILY = "VerifySignatureMlDsa65"

# What ML-DSA-65 verification hashes the message into: mu = SHAKE256(tr || M'),
# tr the 64-byte hash of the public key and, for pure ML-DSA with an empty
# context, M' = 0x00 || 0x00 || M. SHAKE256 absorbs 136 bytes per Keccak
# permutation, so this is the block a per-length price for it would count in.
# Fixed by the algorithm, like SHA256_BLOCK_BYTES.
MLDSA_TR_BYTES = 64
MLDSA_PREFIX_BYTES = 2
SHAKE256_RATE_BYTES = 136

# The block a bytestring copy is proposed in, mirroring gas_cost_table.h's
# kBytestringBlockBytes. Unlike a hash's compression block this is chosen, not
# forced: large enough that rounding a fractional per-byte rate up to a whole
# gas wastes little, small enough that a short write does not pay for a long
# one.
BYTESTRING_BLOCK_BYTES = 512

# The storage rows. frame/ rows time one iteration of "open D frames, issue
# the call on K distinct keys in the innermost, close them all", so they are
# read as a line in K per depth; frame/Push/D is the K=0 point measured
# directly. cold/ rows are one call each, against a real hog_db, and are kept
# out of every schedule number on purpose -- see STORAGE_COLD_NOTE.
FRAME_PUSH_ROW = re.compile(r"^frame/Push/(\d+)$")
FRAME_SWEEP_ROW = re.compile(r"^frame/([A-Za-z0-9]+)/(\d+)/(\d+)$")
FRAME_COMMIT_ROW = re.compile(r"^frame/([A-Za-z0-9]+)(Merge|Discard)/(\d+)$")
COLD_ROW = re.compile(r"^cold/(.+)$")

# A sweep's per-key cost is read at every K, not fitted to one slope: the
# first run showed it rising 1.7x from 1 key to 4096 in a frame, and a line
# through that has a negative intercept and describes nothing. Worst-over-best
# across the sweep beyond this is reported as NOT FLAT, and the schedule is
# built from the dearest point rather than an average.
STORAGE_FLATNESS_TOLERANCE = 1.25

# Gating. Mirrors the opcode calibration's reference in scripts/
# rv32_gas_report.py: costs as ratios to a baseline measured in the same run,
# a loose tolerance, and "a guard that was not measured was not checked".
#
# The tolerance is deliberately the same 2x. This is an alarm for a syscall
# getting wildly more expensive, not a performance budget; a gate that fires on
# ordinary run-to-run drift gets muted and then catches nothing.
GUARD_TOLERANCE = 1.0

# Only rows that reproduce are guarded. A row whose own repetitions disagree by
# more than this cannot distinguish a regression from its own noise, so gating
# on it would fire at random. Recorded per run rather than assumed: the bench
# pool sits near 1% and the development container near 12%.
GUARD_MAX_CV = 0.05

# The baseline every ratio is relative to: the median of the syscalls that do
# nothing but cross the boundary. A median over several rather than one chosen
# syscall, for the reason the opcode reference gives -- one row carrying every
# ratio is one row whose misbehaviour looks like the whole surface moving.
#
# Membership is measured, not listed: a row within this factor of the cheapest
# is doing crossing work and nothing else. The report's own schedule section
# reaches the same grouping from the same data.
GUARD_BASE_SPREAD = 1.5

# A sweep cell is a subtraction: the keys' cost is the iteration minus the
# frames-only baseline. When the frames dominate, that difference is the small
# gap between two large numbers and its relative error is whatever the two
# timings' errors are, magnified. The depth-254 K=1 cell is the example the
# calibration doc calls out -- one ~260 ns touch isolated by subtracting a
# 30 microsecond baseline, which is 4% of what was actually timed. Guarding it
# would gate on the noise in that subtraction rather than on the touch.
GUARD_MIN_SIGNAL_SHARE = 0.10

STORAGE_COLD_NOTE = (
    "cold/ rows miss the snapshot cache on every call and read through the "
    "database named above: the proxy, the RPC, its own lookup, the cache "
    "insert. Against hog_db this is still a floor in one respect -- it is "
    "in-process over a local socket, so a chain whose executor and database "
    "sit on different hosts pays a hop this does not. Excluded from "
    "--schedule until there is a rule for what a cold read is charged to.")


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
    # Keyed the same way rows() keys medians, run_name first and name as the
    # fallback. Google Benchmark emits both, but a hand-built file may carry
    # only one, and a stddev map keyed differently from the medians silently
    # matches nothing -- which reads as "no row reproduces well enough to
    # guard" rather than as the mismatch it is.
    return {(entry.get("run_name") or entry.get("name")): entry
            for entry in data.get("benchmarks", [])
            if entry.get("aggregate_name") == "stddev"}


def context(data):
    return data.get("context", {})


def item_backend(data):
    """What answered the cold rows' misses, or None for a run that did not say.

    cold/ row names did not change when the backend did, so a JSON recorded
    before that carries no claim either way and must not be given this one.
    """
    return context(data).get("syscall_bench_item_backend")


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
        if not name.startswith("entry/"):
            continue
        if (SHA256_ROW.match(name) or BYTESTRING_ROW.match(name) or
                MLDSA_VERIFY_ROW.match(name)):
            continue
        names.append(name[len("entry/"):])
    return sorted(names)


def shake256_blocks(byte_len):
    """Keccak permutations SHAKE256 absorbs byte_len in, padding included.

    pad10*1 needs at least one byte, so a message that fills a block exactly
    still pays for the next one.
    """
    return (byte_len + 1 + SHAKE256_RATE_BYTES - 1) // SHAKE256_RATE_BYTES


def mldsa_message_blocks(message_len):
    """SHAKE256 blocks ML-DSA-65 verification spends hashing the message."""
    return shake256_blocks(MLDSA_TR_BYTES + MLDSA_PREFIX_BYTES + message_len)


def mldsa_verify_summary(measured, sd=None):
    """The ML-DSA-65 sweep, each length beside ed25519's at the same length.

    Returns a list of dicts sorted by message length: the entry row's median
    and stddev, the vm row's ns per call where there is one, the gas the row
    was charged, and the same three for entry/VerifySignature/<length> when
    that row was measured. Each scheme is billed its own cost function
    (GasCost::verify_signature and GasCost::verify_signature_mldsa65), so
    comparing time per gas billed says whether they are priced at the same
    rate -- read off the measurement, without needing a gas unit.
    """
    sd = sd or {}
    points = []
    for name, entry in measured.items():
        match = MLDSA_VERIFY_ROW.match(name)
        if not match or match.group(1) != "entry":
            continue
        size = int(match.group(2))
        vm_row = measured.get(f"vm/{MLDSA_VERIFY_FAMILY}/{size}")
        ed25519 = measured.get(f"entry/VerifySignature/{size}")
        points.append({
            "bytes": size,
            "entry_ns": entry["real_time"],
            "entry_sd": sd.get(name, {}).get("real_time", 0.0),
            "vm_ns": vm_ns_per_call(vm_row) if vm_row else None,
            "gas": int(entry.get("gas", 0)),
            "ed25519_ns": ed25519["real_time"] if ed25519 else None,
            "ed25519_gas": int(ed25519.get("gas", 0)) if ed25519 else None,
        })
    return sorted(points, key=lambda point: point["bytes"])


def print_mldsa_verify(points):
    print()
    print("ML-DSA-65 VerifySignature: billed GasCost::verify_signature_mldsa65,")
    print("kVerifySignatureMlDsa65BaseCost plus kVerifySignatureMlDsa65BlockCost "
          "per SHAKE256 block.")
    header = (f"{'bytes':<10}{'entry ns':>11}{'±':>9}{'gas':>8}"
              f"{'ed25519 ns':>12}{'ed25519 gas':>13}{'x ed25519':>11}")
    print(header)
    print("-" * len(header))
    ratios = []
    for point in points:
        ed_ns = point["ed25519_ns"]
        ed_ns_text = f"{ed_ns:>12.1f}" if ed_ns else f"{'-':>12}"
        ed_gas = point["ed25519_gas"]
        ed_gas_text = f"{ed_gas:>13}" if ed_gas is not None else f"{'-':>13}"
        ratio = point["entry_ns"] / ed_ns if ed_ns else None
        ratio_text = f"{ratio:>10.2f}x" if ratio else f"{'-':>11}"
        # Time per gas against ed25519's, so the claim stays true if the two
        # are ever billed differently.
        if ratio and point["gas"] and ed_gas:
            ratios.append(ratio * ed_gas / point["gas"])
        print(f"{point['bytes']:<10}{point['entry_ns']:>11.1f}"
              f"{point['entry_sd']:>9.2f}{point['gas']:>8}{ed_ns_text}"
              f"{ed_gas_text}{ratio_text}")
    if ratios:
        print(f"Per gas billed, ML-DSA-65 takes {min(ratios):.2f}x .. "
              f"{max(ratios):.2f}x the time ed25519 does at the same length "
              "(1.00x: priced at the same rate).")


def sha256_points(measured, prefix="entry"):
    points = []
    for name, row in measured.items():
        match = SHA256_ROW.match(name)
        if match and match.group(1) == prefix:
            points.append((int(match.group(2)), row["real_time"]))
    return sorted(points)


def fit_line(points, min_points=MIN_SWEEP_POINTS):
    """Least squares over a size sweep: (base_ns, ns_per_byte, r_squared).

    A fit rather than the endpoints, because the endpoints are the two points
    most exposed to a single bad sample and the whole sweep is cheap to use.

    `min_points` is lowered by callers fitting a sub-range that is short by
    construction -- the settled tail of a bytestring sweep is often three
    points. Note that r_squared is not a goodness-of-fit statement when the
    x-values span decades; see top_leverage.
    """
    if len(points) < min_points:
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


def bytestring_points(measured, family, prefix="entry"):
    """(bytes, ns) for one bytestring family's sweep, ascending."""
    points = []
    for name, row in measured.items():
        match = BYTESTRING_ROW.match(name)
        if match and match.group(1) == prefix and match.group(2) == family:
            points.append((int(match.group(3)), row["real_time"]))
    return sorted(points)


def top_leverage(points):
    """The share of a sweep's x-variance carried by its longest point.

    Why R2 is not a goodness-of-fit statement on a sweep like this one. The
    lengths span three decades, so the top point sits enormously far from the
    mean and the fit is very nearly a line through it: R2 then reports whether
    that one point landed near the line, which it does by construction, and
    says nothing about the six points that carry the pricing.
    """
    xs = [float(x) for x, _ in points]
    if len(xs) < 2:
        return None
    mean_x = statistics.fmean(xs)
    var_x = sum((x - mean_x) ** 2 for x in xs)
    if var_x == 0.0:
        return None
    return (max(xs) - mean_x) ** 2 / var_x


def interval_slopes(points):
    """[(lo, hi, ns per byte)] between successive lengths in a sweep.

    The per-byte cost read in each band rather than fitted once across all of
    them -- the same reason the storage sweeps read their per-key cost at
    every K. A bytestring's per-byte cost is not constant: it roughly doubles
    between the short lengths and the long ones as the item stops fitting in
    cache, and one slope through that describes none of the bands. The first
    band, from zero, is not a per-byte cost at all; it is the fixed cost of
    the call having a length, divided by a short length.
    """
    return [(lo, hi, (hi_ns - lo_ns) / (hi - lo))
            for (lo, lo_ns), (hi, hi_ns) in zip(points, points[1:])
            if hi > lo]


def bytestring_summary(measured, sd=None):
    """Per family: the measured base, and the per-byte cost band by band.

    Built from directly measured quantities. A least squares line over the
    whole sweep is carried alongside as a cross-check -- `fit`, and the
    `intercept_error` by which it disagrees with the measured base -- but
    nothing here is derived from it, because on a sweep spanning three decades
    a line describes neither end well while still scoring R2 near 1.

    Only families this run measured; a run from before these rows existed has
    none, and the section is skipped rather than printed empty.
    """
    sd = sd or {}
    summary = {}
    for family in BYTESTRING_FAMILIES:
        points = bytestring_points(measured, family)
        if not points:
            continue
        base = dict(points).get(0)
        intervals = interval_slopes(points)
        fit = fit_line(points)
        entry = {
            "points": points,
            "base_ns": base,
            **bytestring_seam(measured, family),
            "intervals": intervals,
            "flat": bytestring_is_flat(family, points, sd),
            "fit": fit,
            "leverage": top_leverage(points),
        }
        # The first band is the fixed cost of a nonzero length, not a rate.
        if base is not None and points[0][0] == 0 and len(points) > 1:
            entry["first_jump_bytes"] = points[1][0]
            entry["first_jump_ns"] = points[1][1] - base
            rates = [r for lo, _, r in intervals if lo > 0]
        else:
            rates = [r for _, _, r in intervals]
        if rates and min(rates) > 0:
            entry["per_byte_lo"] = min(rates)
            # What a price has to cover. The dearest band, not the average of
            # them -- the same rule the storage schedule builds under.
            entry["per_byte_hi"] = max(rates)
            entry["growth"] = max(rates) / min(rates)
        if fit and base is not None:
            entry["intercept_error"] = fit[0] - base
        summary[family] = entry
    return summary


def bytestring_seam(measured, family):
    """What this family's ecall crossing costs, from its own vm row.

    The bench drives one length end to end per family, so the gap between that
    vm row and the entry row at the same length is this family's crossing --
    measured rather than taken from the table's median, which is a different
    mix of syscalls with a different amount of argument setup ahead of the
    ecall. Empty when the run has no vm row for the family.
    """
    for name, row in measured.items():
        match = BYTESTRING_ROW.match(name)
        if not match or match.group(1) != "vm" or match.group(2) != family:
            continue
        size = int(match.group(3))
        entry_row = measured.get(f"entry/{family}/{size}")
        vm_ns = vm_ns_per_call(row)
        if not entry_row or not vm_ns:
            continue
        return {"seam_ns": vm_ns - entry_row["real_time"],
                "seam_bytes": size,
                "vm_ns": vm_ns,
                "seam_entry_ns": entry_row["real_time"]}
    return {}


def bytestring_is_flat(family, points, sd):
    """Whether a family's cost does not move with length at all.

    Measured, not assumed: `BytestringGetLen` reads an item's length and
    copies none of it, so it should be flat, and a run where it is not is a
    finding rather than a fit to widen. Judged against the row's own stddev
    where the run has repetitions, and against 1% of the base otherwise --
    a single-repetition run reports no deviation and would call everything
    flat on a zero error bar.
    """
    base = dict(points).get(0)
    if base is None or len(points) < 2:
        return False
    top_x, top_ns = points[-1]
    gap = abs(top_ns - base)
    top_sd = sd.get(f"entry/{family}/{top_x}", {}).get("real_time", 0.0)
    base_sd = sd.get(f"entry/{family}/0", {}).get("real_time", 0.0)
    noise = 2.0 * math.hypot(top_sd, base_sd)
    return gap <= max(noise, 0.01 * base)


def print_bytestring(summary, measured, sd):
    """The length sweep, per family, and what can honestly be read off it."""
    print()
    header = (f"{'bytestring bytes':<28}{'entry ns':>11}{'±':>9}"
              f"{'ns/B since prev':>17}{'gas':>8}")
    print(header)
    print("-" * len(header))
    for family, entry in summary.items():
        # The rate since the previous length, not the whole call divided by
        # its length: the base is most of a short call, and dividing it in
        # reports a per-byte cost an order of magnitude off. Reading it band
        # by band is also what makes a rate that grows with length visible in
        # the table rather than buried in one fitted slope.
        rates = {hi: rate for _, hi, rate in entry["intervals"]}
        for size, ns in entry["points"]:
            row_name = f"entry/{family}/{size}"
            error = sd.get(row_name, {}).get("real_time", 0.0)
            gas = int(measured[row_name].get("gas", 0))
            rate = f"{rates[size]:>17.4f}" if size in rates else f"{'-':>17}"
            label = f"{family}/{size}"
            print(f"{label:<28}{ns:>11.1f}{error:>9.2f}{rate}{gas:>8}")

    for family, entry in summary.items():
        print()
        if entry["base_ns"] is None:
            print(f"{family}: no zero-length row, so nothing to read the "
                  "per-byte cost against")
            continue
        print(f"{family}: base {entry['base_ns']:.1f} ns, measured at zero "
              "length")
        if entry.get("seam_ns") is not None:
            # The crossing this family actually pays, from its own vm row
            # rather than from the table's median. A call with more arguments
            # spends longer materialising them before the ecall, so the seam
            # is not one number shared across the surface.
            print(f"  the ecall crossing adds {entry['seam_ns']:.1f} ns at "
                  f"{entry['seam_bytes']} bytes "
                  f"({entry['vm_ns']:.1f} ns/call through the VM against "
                  f"{entry['seam_entry_ns']:.1f} at the entry)")
        if entry["flat"]:
            print("  flat: the longest row is within noise of the base, so "
                  "there is no per-byte cost to price")
        else:
            if "first_jump_ns" in entry:
                jump = entry["first_jump_ns"]
                width = entry["first_jump_bytes"]
                print(f"  the first {width} bytes cost {jump:.1f} ns "
                      f"({jump / width:.4f} ns/byte) -- a fixed cost of the "
                      "call having a length, not a rate")
            if "per_byte_hi" in entry:
                trend = bytestring_trend(entry)
                print(f"  per byte {entry['per_byte_lo']:.4f} .. "
                      f"{entry['per_byte_hi']:.4f} ns across the rest of the "
                      f"sweep ({entry['growth']:.1f}x, {trend})")
                print(f"  a price has to cover the dearest band: "
                      f"{entry['per_byte_hi']:.4f} ns/byte")
            else:
                print("  the per-byte cost does not resolve in this sweep")
        # No cross-check to print for a flat family: there is no slope to
        # check, and an R2 near zero is what a flat line correctly produces.
        fit = None if entry["flat"] else entry["fit"]
        if fit:
            fit_base, per_byte, r_squared = fit
            leverage = entry.get("leverage")
            # Printed as a cross-check; nothing above is derived from it. Two
            # numbers say why not. The intercept has a directly measured
            # counterpart, so where it lands is a real check -- and it is the
            # one that bites, since the fixed first-band cost is folded into
            # it. R2 has no such counterpart on a sweep this wide, and the
            # leverage figure is what that costs.
            detail = f"R2={r_squared:.4f}"
            if leverage is not None:
                detail += (f", but the {entry['points'][-1][0]}-byte point "
                           f"carries {100 * leverage:.0f}% of the x-variance")
            print(f"  one line over the whole sweep would be {fit_base:.1f} "
                  f"ns + {per_byte:.4f} ns/byte ({detail})")
            error = entry.get("intercept_error")
            if error is not None:
                share = 100 * error / entry["base_ns"]
                print(f"  its intercept is {error:+.1f} ns ({share:+.1f}%) "
                      "against the measured base, which is the check R2 "
                      "cannot make")

    # What the pair says. A Get copies the item into guest memory and a GetLen
    # reads the same item without copying it, so the difference is the copy.
    get = summary.get("BytestringGet")
    get_len = summary.get("BytestringGetLen")
    if get and get.get("per_byte_hi") and get_len and get_len["flat"]:
        print()
        print(f"the copy: BytestringGet reaches {get['per_byte_hi']:.4f} "
              "ns/byte, and BytestringGetLen, which reads the same item and "
              "copies none of it, is flat at every length -- so the whole "
              "per-byte cost is the copy")

    # What the schedule charges, against what the sweep measured. None of the
    # three appears in kSyscallGasCostSchedule, so each is billed
    # kSyscallBaseCost and nothing per byte -- and ns/gas across the sweep is
    # how far apart a flat charge and a growing cost end up, in the same terms
    # the SHA256 section reports its own spread.
    print()
    for family, entry in summary.items():
        rates = []
        charged = set()
        for size, ns in entry["points"]:
            gas = int(measured[f"entry/{family}/{size}"].get("gas", 0))
            charged.add(gas)
            if gas:
                rates.append(ns / gas)
        if not rates:
            print(f"{family}: charged nothing at any length")
        elif len(charged) == 1:
            print(f"{family}: charged {charged.pop()} gas at every length; "
                  f"ns/gas runs {min(rates):.4f} .. {max(rates):.4f} "
                  f"({max(rates) / min(rates):.0f}x spread within one "
                  "syscall)")
        else:
            print(f"{family}: ns/gas runs {min(rates):.4f} .. "
                  f"{max(rates):.4f}")


def bytestring_trend(entry):
    """How the per-byte cost moves across the sweep, as a phrase.

    Which direction it moves is the finding, not a detail: a cost that grows
    with length cannot be priced from its cheap end, and one that shrinks
    cannot be priced from its dear end without overcharging every small write.
    """
    if entry["growth"] <= STORAGE_FLATNESS_TOLERANCE:
        return "flat within tolerance"
    rates = [rate for lo, _, rate in entry["intervals"] if lo > 0]
    if len(rates) >= 2 and rates[-1] > rates[0]:
        return "RISING with length"
    if len(rates) >= 2 and rates[-1] < rates[0]:
        return "FALLING with length"
    return "NOT FLAT"


def warm_syscall_for(family):
    """The warm syscall row a first-touch family is the surcharge over.

    `NNIntGetFirstTouch` is `NNIntGet` issued in a state where the frame holds
    no view yet, so what it costs beyond the warm row is the first touch, and
    the warm part is already priced in the syscall table.
    """
    return family.replace("FirstTouch", "")


def storage_push_baselines(measured):
    """frame/Push/D rows: {depth: ns per iteration}."""
    baselines = {}
    for name, row in measured.items():
        match = FRAME_PUSH_ROW.match(name)
        if match:
            baselines[int(match.group(1))] = row["real_time"]
    return baselines


def storage_sweeps(measured):
    """frame/FAMILY/D/K rows: {family: {depth: [(keys, ns per iteration)]}}.

    Commit rows (frame/FAMILYMerge/K) are not sweeps in this sense and are
    handled by storage_commit_pairs.
    """
    sweeps = {}
    for name, row in measured.items():
        match = FRAME_SWEEP_ROW.match(name)
        if not match:
            continue
        family, depth, keys = match.group(1), int(match.group(2)), \
            int(match.group(3))
        sweeps.setdefault(family, {}).setdefault(depth, []).append(
            (keys, row["real_time"]))
    for family in sweeps.values():
        for depth in family:
            family[depth].sort()
    return sweeps


def storage_commit_pairs(measured):
    """frame/FAMILYMerge/K against frame/FAMILYDiscard/K:
    {family: [(keys, merge_ns, discard_ns)]}.

    The two rows do identical work up to the child's close, so their
    difference over K is what committing one staged item into a parent costs.
    """
    merges, discards = {}, {}
    for name, row in measured.items():
        match = FRAME_COMMIT_ROW.match(name)
        if not match:
            continue
        family, kind, keys = match.group(1), match.group(2), int(match.group(3))
        (merges if kind == "Merge" else discards).setdefault(family, {})[keys] \
            = row["real_time"]
    pairs = {}
    for family, by_keys in merges.items():
        for keys, merge_ns in sorted(by_keys.items()):
            discard_ns = discards.get(family, {}).get(keys)
            if discard_ns is not None:
                pairs.setdefault(family, []).append((keys, merge_ns, discard_ns))
    return pairs


def cold_rows(measured, sd):
    """cold/NAME rows: {name: (ns per call, stddev)}.

    The cold row registers with a fixed iteration count, which Google
    Benchmark appends to the name as `/iterations:N`; that is a run parameter
    and not part of the syscall's name, so it is dropped.
    """
    out = {}
    for name, row in measured.items():
        match = COLD_ROW.match(name)
        if match:
            syscall = match.group(1).split("/iterations:", 1)[0]
            out[syscall] = (row["real_time"],
                            sd.get(name, {}).get("real_time", 0.0))
    return out


def storage_summary(measured, sd):
    """Everything the storage tables and the schedule need, or None when the
    run has no storage rows.

    Per sweep family and depth: the per-key cost at every K against the push
    baseline, the dearest of them, and whether the sweep is flat. Per commit
    family: the per-item merge cost at each K. The cold rows verbatim.
    """
    baselines = storage_push_baselines(measured)
    sweeps = storage_sweeps(measured)
    pairs = storage_commit_pairs(measured)
    cold = cold_rows(measured, sd)
    if not (baselines or sweeps or pairs or cold):
        return None
    summary = {"baselines": baselines, "sweeps": {}, "commits": {},
               "cold": cold}
    for family, by_depth in sweeps.items():
        for depth, points in by_depth.items():
            baseline = baselines.get(depth)
            entry = {"points": points, "baseline_ns": baseline}
            if baseline is None:
                summary["sweeps"].setdefault(family, {})[depth] = entry
                continue
            # Per-key cost at each K: the iteration minus the measured
            # frames-only baseline, over the keys. A point per K rather than
            # one slope, because the cost is not constant in K -- the frame's
            # map grows, rehashes, and stops fitting in cache -- and a schedule
            # has to hold at the dearest point, which is what "worst" is.
            per_key = [(keys, (ns - baseline) / keys)
                       for keys, ns in points if keys > 0]
            entry["per_key"] = per_key
            costs = [cost for _, cost in per_key if cost > 0]
            if costs:
                entry["worst_ns"] = max(costs)
                entry["worst_at"] = max(per_key, key=lambda p: p[1])[0]
                entry["spread"] = max(costs) / min(costs)
                entry["flat"] = entry["spread"] <= STORAGE_FLATNESS_TOLERANCE
                # What the first touch adds to the same syscall made warm.
                # The row above is the whole call -- crossing, ABI parse,
                # handler and all -- because only the frames-only baseline was
                # subtracted from it, and every one of those is already priced
                # in the syscall table. A schedule that charged this row on top
                # of that table would bill the warm call twice, so what a
                # first-touch constant may carry is the difference.
                warm = measured.get("entry/" + warm_syscall_for(family))
                if warm:
                    entry["warm_ns"] = warm["real_time"]
                    entry["surcharge_ns"] = max(
                        0.0, entry["worst_ns"] - warm["real_time"])
            summary["sweeps"].setdefault(family, {})[depth] = entry
    for family, rows_ in pairs.items():
        summary["commits"][family] = [
            (keys, merge_ns, discard_ns, (merge_ns - discard_ns) / keys)
            for keys, merge_ns, discard_ns in rows_ if keys > 0]
    return summary


def parent_walk_ns(summary, family="NNIntGetFirstTouch"):
    """What each additional parent frame adds to a first touch: the per-key
    cost at the deepest measured depth minus the shallowest, per frame of
    difference, at the largest K both were measured at. None when the family
    was not swept over depth.

    Returns (ns per frame, shallow depth, deep depth, K).
    """
    by_depth = summary["sweeps"].get(family, {})
    depths = sorted(d for d, e in by_depth.items() if e.get("per_key"))
    if len(depths) < 2:
        return None
    shallow, deep = depths[0], depths[-1]
    shallow_costs = dict(by_depth[shallow]["per_key"])
    deep_costs = dict(by_depth[deep]["per_key"])
    common = sorted(set(shallow_costs) & set(deep_costs))
    if not common:
        return None
    keys = common[-1]
    return ((deep_costs[keys] - shallow_costs[keys]) / (deep - shallow),
            shallow, deep, keys)


def print_storage(summary, unit_ns=None, backend=None):
    """The storage tables. With a unit, costs are also shown in gas."""
    def gas(ns):
        return f"{ns / unit_ns:>8.1f}" if unit_ns else f"{'-':>8}"

    if summary["baselines"]:
        print()
        header = (f"{'frames opened+discarded':<28}{'ns/iteration':>14}"
                  f"{'ns/frame':>10}{'gas/frame':>10}")
        print(header)
        print("-" * len(header))
        for depth, ns in sorted(summary["baselines"].items()):
            print(f"{depth:<28}{ns:>14.1f}{ns / depth:>10.1f}"
                  f"{gas(ns / depth):>10}")

    for family, by_depth in sorted(summary["sweeps"].items()):
        print()
        # Depths need not share a key sweep -- the deep one runs only where
        # the question is the frame map rather than the parent walk -- so the
        # columns are the union and a depth that skipped a point leaves it
        # blank rather than shifting its row under the wrong heading.
        all_keys = sorted({k for entry in by_depth.values()
                           for k, _ in entry.get("per_key", [])})
        header = (f"{family:<28}{'depth':>6}  ns/key at K = "
                  + " ".join(f"{k:>8}" for k in all_keys)
                  + f"{'worst':>10}{'gas':>8}{'spread':>8}")
        print(header)
        print("-" * len(header))
        for depth, entry in sorted(by_depth.items()):
            if not entry.get("per_key"):
                print(f"{'':<28}{depth:>6}  (no frame/Push/{depth} baseline "
                      "to subtract)")
                continue
            # A per-key cost at or below zero is a point where the frames-only
            # baseline is large and noisy next to the few calls being isolated
            # (one touch against 254 frames); it is not a cost, so it is not
            # printed as one and does not enter the worst or the spread.
            by_k = dict(entry["per_key"])
            cells = " ".join(
                (f"{by_k[k]:>8.1f}" if by_k[k] > 0 else f"{'<noise':>8}")
                if k in by_k else f"{'':>8}"
                for k in all_keys)
            flag = "" if entry.get("flat", True) else "  <- NOT FLAT"
            print(f"{'':<28}{depth:>6}{'':>15}{cells}"
                  f"{entry['worst_ns']:>10.1f}{gas(entry['worst_ns'])}"
                  f"{entry['spread']:>7.2f}x{flag}")
    if summary["sweeps"]:
        print()
        print("ns/key is (iteration - frame/Push/depth) / K: what each of K "
              "first touches in one")
        print("frame costs, at that K -- the whole syscall, not the part that "
              "is new. NOT FLAT marks")
        print(f"a spread past {STORAGE_FLATNESS_TOLERANCE:g}x; the dearest "
              "point is the one a schedule has to cover.")
        for family, by_depth in sorted(summary["sweeps"].items()):
            for depth, entry in sorted(by_depth.items()):
                if "surcharge_ns" not in entry:
                    continue
                print(f"  {family} at depth {depth} is "
                      f"{entry['worst_ns']:.1f} ns, of which "
                      f"{entry['warm_ns']:.1f} is the warm "
                      f"{warm_syscall_for(family)} the syscall table already "
                      f"prices;")
                print(f"    the first touch itself is "
                      f"{entry['surcharge_ns']:.1f} ns.")
                break
    walk = parent_walk_ns(summary)
    if walk:
        per_frame, shallow, deep, keys = walk
        print()
        print(f"parent walk: a first touch costs {per_frame:.1f} ns more per "
              f"frame of depth (depth {shallow} to {deep}, at K={keys}).")
        print("Every miss in the active frame looks in every parent before "
              "creating a view, so this is")
        print("paid on every first touch whether or not any parent holds the "
              "key, and it grows with")
        print("whatever max_cross_invoke_depth the chain sets.")

    for family, rows_ in sorted(summary["commits"].items()):
        print()
        header = (f"{family + ' commit':<28}{'K':>6}{'merge ns':>11}"
                  f"{'discard ns':>12}{'ns/item':>9}{'gas/item':>9}")
        print(header)
        print("-" * len(header))
        for keys, merge_ns, discard_ns, per_item in rows_:
            print(f"{'':<28}{keys:>6}{merge_ns:>11.1f}{discard_ns:>12.1f}"
                  f"{per_item:>9.1f}{gas(per_item):>9}")
        print("Merge and Discard are the same writes in a child frame; Merge "
              "commits them into a fresh")
        print("parent and Discard drops them. The difference is what a "
              "successful child costs its parent")
        print("per staged item, which is where a write's in-executor cost "
              "lands rather than in the syscall.")

    if summary["cold"]:
        print()
        # Named by the run rather than by the row: cold/ row names did not
        # change when the backend did, so a JSON recorded before that carries
        # no claim either way and must not be given this one.
        label = f"cold ({backend or 'backend unrecorded'})"
        header = f"{label:<28}{'ns/call':>10}{'±':>9}"
        print(header)
        print("-" * len(header))
        for name, (ns, err) in sorted(summary["cold"].items()):
            print(f"{name:<28}{ns:>10.1f}{err:>9.2f}")
        print(textwrap.fill(STORAGE_COLD_NOTE, width=78))


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
    return baseline_ns(data)


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
#
# ML-DSA-65 is the same kind of check as ed25519: hogd verifies with
# mldsa-native and OpenSSL 3.5+ has its own implementation, so a factor apart
# is allowed and orders of magnitude is the finding. OpenSSL before 3.5 has no
# ML-DSA at all; the row is then simply absent, not failed.
SHA256_CROSSCHECK_TOLERANCE = 0.25
ED25519_CROSSCHECK_FACTOR = 10.0
MLDSA_CROSSCHECK_FACTOR = 10.0


def parse_openssl_speed(data):
    """Rates out of `openssl speed -mr` output.

    Returns {"sha256": {block_size: bytes_per_second}, "ed25519": {...}}.

    Reads the machine-readable +H/+F lines rather than the human table, which
    has been relaid out more than once across OpenSSL releases.
    """
    sizes = []
    sha256 = {}
    ed25519 = {}
    mldsa65 = {}
    for line in data.get("raw", []):
        fields = line.split(":")
        # ML-DSA-65 first, matched by name rather than by line tag: OpenSSL
        # 3.5 reports its signature algorithms on a +F<n> line of their own,
        # and the tag number is the least stable thing about it. The rates
        # follow the name, verifications per second last.
        names = [f.replace("-", "").lower() for f in fields]
        if fields[0].startswith("+F") and "mldsa65" in names:
            rest = fields[names.index("mldsa65") + 1:]
            rates = []
            for value in rest:
                try:
                    rates.append(float(value))
                except ValueError:
                    continue
            if rates:
                mldsa65 = {"verify_per_second": rates[-1]}
            continue
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
    return {"sha256": sha256, "ed25519": ed25519, "mldsa65": mldsa65}


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
    fit = fit_line(sha256_points(measured))
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

    mldsa_verify = speed.get("mldsa65", {}).get("verify_per_second")
    ours = [point["entry_ns"] for point in mldsa_verify_summary(measured)]
    if mldsa_verify and ours:
        # The cheapest row, as for ed25519: openssl times a short message.
        ours_ns = min(ours)
        openssl_ns = 1e9 / mldsa_verify
        ratio = ours_ns / openssl_ns
        low = 1.0 / MLDSA_CROSSCHECK_FACTOR
        ok = low <= ratio <= MLDSA_CROSSCHECK_FACTOR
        lines.append(("ML-DSA-65 verify ns", f"{openssl_ns:.0f}",
                      f"{ours_ns:.0f}", ratio, ok,
                      "hogd verifies with mldsa-native and openssl with its "
                      "own implementation, so a factor apart is allowed -- "
                      "an order of magnitude is the test"))

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

    mldsa = mldsa_verify_summary(measured, sd)
    if mldsa:
        print_mldsa_verify(mldsa)

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
        fit = fit_line(points)
        if fit:
            base, per_byte, r_squared = fit
            print()
            print(f"measured: {base:.1f} ns + {per_byte:.4f} ns/byte "
                  f"(R2={r_squared:.4f})")
            print(f"charged:  {COMMITTED_SYSCALL_BASE_GAS} base + "
                  f"{COMMITTED_SHA256_BLOCK_GAS} gas per "
                  f"{SHA256_BLOCK_BYTES}-byte block  (SHA256HashCost)")
            rates = [ns / int(measured[f'entry/SHA256/{size}'].get('gas', 0))
                     for size, ns in points
                     if int(measured[f"entry/SHA256/{size}"].get("gas", 0))]
            if rates:
                print(f"ns/gas across the sweep: {min(rates):.4f} .. "
                      f"{max(rates):.4f} ({max(rates) / min(rates):.0f}x "
                      "spread within one syscall)")

    bytestrings = bytestring_summary(measured, sd)
    if bytestrings:
        print_bytestring(bytestrings, measured, sd)

    storage = storage_summary(measured, sd)
    if storage:
        print_storage(storage, backend=item_backend(data))

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
COMMITTED_SHA256_BLOCK_GAS = 22
COMMITTED_SHA256_STREAM_GAS = 26
COMMITTED_MLDSA_VERIFY_BASE_GAS = 46592
COMMITTED_MLDSA_VERIFY_BLOCK_GAS = 160


def sha256_blocks(byte_len):
    """Compression blocks a SHA-256 of this length performs, padding included."""
    return (byte_len + SHA256_PAD_BYTES + SHA256_BLOCK_BYTES - 1) \
        // SHA256_BLOCK_BYTES


def committed_sha256_gas(byte_len):
    return (COMMITTED_SYSCALL_BASE_GAS
            + COMMITTED_SHA256_BLOCK_GAS * sha256_blocks(byte_len))


def fit_sha256_blocks(points):
    """Least squares against compression blocks: (base_ns, ns_per_block, r2).

    The block is what SHA-256 actually does. R2 is reported but is close to
    useless as a verdict here: the sweep spans 1 to 513 blocks, so one point
    carries two thirds of the x-variance and any roughly linear model scores
    ~1.0. The diagnostic that discriminates is the billed-to-measured ratio
    across the sweep, printed by --schedule.

    The intercept is reported for the same reason and trusted no further. Every
    point hashes at least one block, since SHA-256 always compresses its
    padding, so base_ns extrapolates to a block count no call can have.

    Fitting the block is also what removes the need for a rate denominator: a
    per-byte price is a fraction no integer can express, and the divisor that
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
    unit = gas_unit_ns(baseline, resolution)
    ecall_gas = ECALL_INSTRUCTION_GAS * resolution

    def to_gas(ns):
        # Round up, never down. A syscall priced below what it costs is
        # capacity the schedule hands out for free, and the rounding error is
        # bounded by one unit either way -- so take the safe direction, the
        # same way the opcode schedule does.
        return max(1, math.ceil(ns * (1.0 + margin) / unit - 1e-9))

    proposal = {"unit_ns": unit, "baseline_ns": baseline,
                "resolution": resolution, "ecall_gas": ecall_gas,
                "margin": margin, "syscalls": {}}

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
               "total_gas": to_gas(row["vm_ns"]) - ecall_gas,
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
    proposal["base_gas"] = max(1, to_gas(worst_ns) - ecall_gas)
    proposal["base_ns"] = worst_ns
    proposal["base_from"] = worst_name

    fit = fit_line(sha256_points(measured))
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

    storage = storage_summary(measured, stddevs(bench))
    if storage:
        proposal["storage"] = storage
        # The shape the measurement suggests, in gas. Each is the slope of a
        # sweep, so the crossing is not in it: a first touch is priced as the
        # warm call (which is in the syscall table above) plus this.
        first = storage["sweeps"].get("NNIntGetFirstTouch", {})
        shallow = min(first) if first else None
        if shallow is not None and "surcharge_ns" in first[shallow]:
            # The surcharge over the warm call, at the shallowest depth and the
            # dearest K: the per-frame walk is priced separately, and the cost
            # in K is not flat, so worst-case pricing takes the worst K. The
            # warm call itself is already in the syscall table, so only the
            # difference belongs here.
            proposal["storage_first_touch_gas"] = to_gas(
                first[shallow]["surcharge_ns"])
            proposal["storage_first_touch_warm_ns"] = first[shallow]["warm_ns"]
            proposal["storage_first_touch_from"] = warm_syscall_for(
                "NNIntGetFirstTouch")
        # The parent walk is measured and reported, never proposed. Since
        # #3771 it is a fraction of a nanosecond per frame, which rounds to
        # 1 gas -- below what this benchmark resolves, and a constant in this
        # table is not free: every one is hashed into
        # rv32::kGasScheduleDigest, so adding one buys a coordinated
        # gas-schedule update for a term that is noise. What is left unpriced
        # is bounded by what it costs to get deep in the first place: reaching
        # depth D means D nested invokes, each dearer than the walk it adds.
        # The number is printed so a regression -- or a chain setting
        # max_cross_invoke_depth far higher -- is visible in the sweep.
        walk = parent_walk_ns(storage)
        if walk:
            proposal["storage_parent_frame_ns"] = walk[0]
            proposal["storage_parent_frame_unpriced_gas"] = (
                walk[0] * walk[2] / unit)
        commits = storage["commits"].get("NNIntSetAdd", [])
        if commits:
            proposal["storage_commit_item_gas"] = to_gas(
                max(per_item for _, _, _, per_item in commits))

    bytestrings = bytestring_summary(measured, stddevs(bench))
    if bytestrings:
        proposal["bytestrings"] = bytestrings
        # A block rather than a per-byte rate: the measured rate is a fraction
        # of a gas unit, and a fraction needs a divisor, which is one more
        # billing-relevant constant in the digest. Sized to the dearest band,
        # because the per-byte cost is not constant across the sweep and there
        # is no single rate to be right about -- the same rule the storage
        # numbers above are proposed under.
        blocks = {}
        for family, entry in bytestrings.items():
            if entry["flat"] or not entry.get("per_byte_hi"):
                continue
            per_block_ns = entry["per_byte_hi"] * BYTESTRING_BLOCK_BYTES
            blocks[family] = {
                "block_bytes": BYTESTRING_BLOCK_BYTES,
                "per_byte_ns": entry["per_byte_hi"],
                "block_gas": to_gas(per_block_ns),
                # What the rounding to a whole gas costs. A block small enough
                # that this is large is a block that overcharges by rounding
                # alone, which is the reason not to pick the hash's 64.
                "rounding": (to_gas(per_block_ns) * unit) / per_block_ns,
            }
        if blocks:
            proposal["bytestring_blocks"] = blocks

    # ML-DSA-65 is billed by its own two constants, so its estimate is kept
    # under its own key: nothing above reads it, and it never moves the shared
    # base. Computed here rather than in the printer for the reason the heavy
    # rows are -- to_gas is where --margin applies.
    mldsa = mldsa_verify_summary(measured, stddevs(bench))
    if mldsa:
        points = []
        for point in mldsa:
            # What a guest causes, crossing included: the vm row where there is
            # one, otherwise the entry row plus this run's seam -- the same
            # quantity the heavy rows and the SHA256 sweep are priced on.
            guest_ns = point["vm_ns"] or (point["entry_ns"] +
                                          proposal["seam_ns"])
            measured_gas = to_gas(guest_ns) - ecall_gas
            points.append({**point, "guest_ns": guest_ns,
                           "blocks": mldsa_message_blocks(point["bytes"]),
                           "measured_gas": measured_gas,
                           "underpriced": (measured_gas / point["gas"]
                                           if point["gas"] else None)})
        estimate = {"points": points}
        # base + per SHAKE256 block, the shape a price of its own would take.
        # Over the entry rows plus the seam, so every length is measured the
        # same way; two points suffice because the sweep is three lengths and
        # the block count is what varies.
        fit = fit_line([(p["blocks"], p["entry_ns"] + proposal["seam_ns"])
                        for p in points], min_points=2)
        if fit:
            base_ns, per_block_ns, r_squared = fit
            block_gas = to_gas(per_block_ns) if per_block_ns > 0 else 0
            # The constant the syscall adds to the shared base: the smallest
            # that bills every measured length at least what it measured.
            own_base_gas = max(1, max(
                p["measured_gas"] - proposal["base_gas"]
                - block_gas * p["blocks"] for p in points))
            estimate.update({
                "base_ns": base_ns,
                "per_block_ns": per_block_ns,
                "r_squared": r_squared,
                "base_gas": max(1, to_gas(base_ns) - ecall_gas),
                "block_gas": block_gas,
                "own_base_gas": own_base_gas,
            })
        proposal["mldsa_verify_estimate"] = estimate
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
    budget_ns = ((proposal["base_gas"] + proposal["ecall_gas"]) *
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
    print(f"({proposal['ecall_gas']} gas of it is already charged as the ecall")
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
            proposed = (proposal["base_gas"] +
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

    storage = proposal.get("storage")
    if storage:
        print()
        print("Storage (NNInt), in this run's gas unit:")
        print_storage(storage, unit_ns=proposal["unit_ns"],
                      backend=item_backend(bench))

    estimate = proposal.get("mldsa_verify_estimate")
    if estimate:
        print()
        print("ML-DSA-65 VerifySignature, billed by its own constants "
              "(kVerifySignatureMlDsa65BaseCost,")
        print(f"  committed {COMMITTED_MLDSA_VERIFY_BASE_GAS}, and "
              f"kVerifySignatureMlDsa65BlockCost, committed "
              f"{COMMITTED_MLDSA_VERIFY_BLOCK_GAS}). Kept")
        print("  apart from the shared constants below. measured/billed above "
              "1.00x means")
        print("  the committed price is below what this run measured.")
        print(f"    {'bytes':>8}{'ns/call':>11}{'gas now':>9}"
              f"{'gas measured':>14}{'measured/billed':>17}")
        for point in estimate["points"]:
            under = point["underpriced"]
            under_text = f"{under:>16.2f}x" if under else f"{'-':>17}"
            print(f"    {point['bytes']:>8}{point['guest_ns']:>11.1f}"
                  f"{point['gas']:>9}{point['measured_gas']:>14}{under_text}")
        if "block_gas" in estimate:
            print(f"  As a price of its own, the sweep fits "
                  f"{estimate['base_ns']:.1f} ns + "
                  f"{estimate['per_block_ns']:.1f} ns per "
                  f"{SHAKE256_RATE_BYTES}-byte SHAKE256 block")
            print(f"  (R2={estimate['r_squared']:.4f}): about "
                  f"{estimate['base_gas']} gas + {estimate['block_gas']} per "
                  "block, shared base included.")
            print(f"  Over this run's shared base of {proposal['base_gas']}: "
                  f"kVerifySignatureMlDsa65BaseCost = "
                  f"{estimate['own_base_gas']},")
            print(f"  kVerifySignatureMlDsa65BlockCost = "
                  f"{estimate['block_gas']}.")

    print()
    print("=" * 68)
    print("Proposed hogd/hog_exec/runtime/gas_cost_table.h constants")
    print("=" * 68)
    print(f"constexpr static uint64_t kSyscallBaseCost = "
          f"{proposal['base_gas']};")
    if sha and "block_gas" in sha:
        print(f"constexpr static uint64_t kSHA256HashBlockCost = "
              f"{sha['block_gas']};"
              f"  // committed: {COMMITTED_SHA256_BLOCK_GAS}")
        print()
        # Reported, not proposed. The schedule charges no per-call base for
        # SHA256: every call compresses at least its padding block, so this
        # extrapolates below the sweep and is not a quantity the run measures.
        # Watch it anyway -- if it ever grows past a few gas, the block model
        # has stopped describing the syscall and the schedule should say so.
        print(f"  fit residual over the crossing and the first block: "
              f"{sha['block_base_gas']} gas "
              f"({sha['block_base_ns']:.1f} ns intercept, "
              f"crossing {proposal['base_ns']:.1f} ns). Not charged.")
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
    blocks = proposal.get("bytestring_blocks")
    if blocks:
        print()
        print("  Bytestrings, priced per block rather than per byte: the")
        print(f"  measured rate is a fraction of the {unit:.4f} ns gas unit,")
        print("  and a fraction needs a divisor, which is one more constant")
        print("  in the digest.")
        print(f"    kBytestringBlockBytes = {BYTESTRING_BLOCK_BYTES};")
        for family, block in sorted(blocks.items()):
            name = ("kBytestringWriteBlockCost" if family == "BytestringSet"
                    else "kBytestringReadBlockCost")
            print(f"    {name} = {block['block_gas']};"
                  f"   // {family}, {block['per_byte_ns']:.4f} ns/byte at its "
                  "dearest band")
            print(f"      rounding a whole gas onto that block costs "
                  f"{block['rounding']:.2f}x.")
        flat = [f for f, e in proposal["bytestrings"].items() if e["flat"]]
        if flat:
            print(f"    {', '.join(sorted(flat))}: no block term -- flat in "
                  "length, so the storage access is the whole cost.")

    if storage:
        print()
        print("  Storage, measured and not yet priced. These are charges on")
        print("  top of the syscall table above, not replacements for it:")
        if "storage_first_touch_gas" in proposal:
            print(f"    kStorageFirstTouchCost = "
                  f"{proposal['storage_first_touch_gas']};   // new: per key "
                  "this tx has not touched, snapshot-cached")
            print(f"      the surcharge over a warm "
                  f"{proposal['storage_first_touch_from']} "
                  f"({proposal['storage_first_touch_warm_ns']:.1f} ns), which "
                  "the table already prices.")

        if "storage_commit_item_gas" in proposal:
            print(f"    kStorageCommitItemCost = "
                  f"{proposal['storage_commit_item_gas']};   // new: per "
                  "staged item a successful child hands its parent")
        if "storage_parent_frame_ns" in proposal:
            print(f"  The parent walk is not proposed: "
                  f"{proposal['storage_parent_frame_ns']:.2f} ns per open "
                  "frame rounds to 1 gas,")
            print("  which is under what this benchmark resolves, and every "
                  "constant here is")
            print("  hashed into the gas-schedule digest. At the deepest "
                  "depth swept that")
            print(f"  leaves ~"
                  f"{proposal['storage_parent_frame_unpriced_gas']:.0f} gas "
                  "unpriced per first touch, bounded by the nested")
            print("  invokes needed to get there, each dearer than the walk "
                  "it adds. The")
            print("  sweep still measures it, so a regression shows up.")
        print("  A first touch is a fact about the transaction's own history")
        print("  -- deterministic, since it depends only on this tx's earlier")
        print("  syscalls -- and not about the block's cache, which is not.")
        print("  Whether that state-dependence belongs in the schedule is the")
        print("  decision the numbers are input to. The cold row is not")
        print(textwrap.fill("proposed: " + STORAGE_COLD_NOTE.split(". ", 1)[1],
                            width=78, initial_indent="  ",
                            subsequent_indent="  "))
    print()
    print("Changing any of these is a coordinated gas-schedule update: each")
    print("is an entry in kSyscallGasCostSchedule, which is folded into")
    print("rv32::kGasScheduleDigest -- so the pinned digest in artifact.h, the")
    print("artifact format fingerprint derived from it, and any artifact")
    print("stamped with the current schedule all move in the same change.")
    return 0


def guard_baseline(measured, sd):
    """The crossing cost this run's ratios are relative to, or None.

    The median of the entry rows within GUARD_BASE_SPREAD of the cheapest --
    the syscalls that only cross the boundary. Returns None when fewer than
    three qualify, since a median over two is one of the two.
    """
    costs = []
    for name in syscall_names(measured):
        row = measured.get("entry/" + name)
        if row and row.get("real_time", 0) > 0:
            costs.append(row["real_time"])
    if len(costs) < 3:
        return None
    # Anchored on the median of the three cheapest rather than on the single
    # cheapest. One anomalously fast row -- a syscall that returns early, a
    # measurement that went wrong -- would otherwise drag the anchor below the
    # crossing cluster and exclude the very rows the baseline is meant to be.
    anchor = statistics.median(sorted(costs)[:3])
    base = [c for c in costs if c <= anchor * GUARD_BASE_SPREAD]
    if len(base) < 3:
        return None
    return statistics.median(base)


def guard_cv(name, measured, sd):
    """Coefficient of variation for a row, or None when it cannot be read."""
    row = measured.get(name)
    dev = sd.get(name)
    if not row or not dev or not row.get("real_time"):
        return None
    return dev.get("real_time", 0.0) / row["real_time"]


def guardable(measured, sd):
    """Every quantity worth gating, as {key: ratio-to-baseline}.

    Three families. `syscall/<name>` is an entry row: the production path for
    one call, which is what a contract pays. `storage/<family>/<depth>/<keys>`
    is a per-key cost from a sweep, which is what it pays per key it has not
    touched -- the term the syscall rows cannot see.
    `bytestring/<family>/<bytes>` is one point of a length sweep, gated at
    every length because the per-byte cost is not constant across it: a
    regression confined to the long end is invisible in the short rows, and it
    is the long end the block term is priced from.

    The bytestring rows are gated here rather than through syscall_names, which
    they stay out of -- that set also drives the syscall table's layout and
    --schedule, and a per-length "syscall" is neither. Their presence does not
    disturb the baseline: it is the median of the rows within GUARD_BASE_SPREAD
    of the cheapest, and the cheapest bytestring row sits well above that.

    cold/ rows are deliberately absent: nothing charges for a cold read yet,
    and a guard on an unpriced row gates noise. The ML-DSA-65 verify rows are
    present, under syscall/, for the opposite reason: see the comment at the
    loop that adds them.
    """
    baseline = guard_baseline(measured, sd)
    if not baseline:
        return None, {}
    guards = {}
    for name in syscall_names(measured):
        row_name = "entry/" + name
        row = measured.get(row_name)
        cv = guard_cv(row_name, measured, sd)
        if not row or cv is None or cv > GUARD_MAX_CV:
            continue
        guards[f"syscall/{name}"] = row["real_time"] / baseline
    summary = storage_summary(measured, sd)
    for family, by_depth in sorted((summary or {}).get("sweeps", {}).items()):
        for depth, entry in sorted(by_depth.items()):
            for keys, per_key in entry.get("per_key", []):
                if per_key <= 0:
                    continue
                row_name = f"frame/{family}/{depth}/{keys}"
                cv = guard_cv(row_name, measured, sd)
                if cv is None or cv > GUARD_MAX_CV:
                    continue
                iteration = measured[row_name]["real_time"]
                if (per_key * keys) / iteration < GUARD_MIN_SIGNAL_SHARE:
                    continue
                guards[f"storage/{family}/{depth}/{keys}"] = per_key / baseline
    for family, entry in sorted(bytestring_summary(measured, sd).items()):
        for size, ns in entry["points"]:
            row_name = f"entry/{family}/{size}"
            cv = guard_cv(row_name, measured, sd)
            if cv is None or cv > GUARD_MAX_CV:
                continue
            guards[f"bytestring/{family}/{size}"] = ns / baseline
    # ML-DSA-65 verify, at every length. Unlike a cold read it IS priced: a
    # contract can call it and pays GasCost::verify_signature_mldsa65. An
    # engine regression would open a gap between that price and the cost with
    # no constant moving -- the one change this gate exists to catch and the
    # digest cannot. So it is guarded like
    # any priced syscall, under the same syscall/ key the ed25519 rows use.
    # Kept out of syscall_names (see MLDSA_VERIFY_ROW), so added here; the
    # guard baseline never sees it, and the cheapest ML-DSA row is hundreds of
    # times the crossing anyway.
    for point in mldsa_verify_summary(measured, sd):
        row_name = f"entry/{MLDSA_VERIFY_FAMILY}/{point['bytes']}"
        cv = guard_cv(row_name, measured, sd)
        if cv is None or cv > GUARD_MAX_CV:
            continue
        guards[f"syscall/{MLDSA_VERIFY_FAMILY}/{point['bytes']}"] = (
            point["entry_ns"] / baseline)
    return baseline, guards


def emit_reference(data):
    """Write the syscall reference a later run is checked against."""
    measured = rows(data)
    sd = stddevs(data)
    ctx = context(data)
    baseline, guards = guardable(measured, sd)
    if not guards:
        print("no guardable rows in this run", file=sys.stderr)
        return 1
    print("# RV32 syscall cost reference.")
    print("#")
    print("# Regenerate with:")
    print("#   scripts/rv32_syscall_bench.sh BUILD_DIR --out /tmp/sys.json")
    print("#   scripts/rv32_syscall_report.py /tmp/sys.json --emit-reference \\")
    print("#     > hogd/hog_exec/runtime/rv32/gas_calibration/"
          "syscall_reference.txt")
    print("#")
    print("# Or run the RV32 Gas Calibration workflow with mode=reference,")
    print("# which prints this block from the bench host.")
    print("#")
    print("# Checked by scripts/rv32_syscall_report.py --check. Costs are")
    print("# multiples of this run's own crossing cost, not nanoseconds, so")
    print("# the file survives the bench host changing speed. It does not")
    print("# survive changing microarchitecture: a syscall that waits on")
    print("# memory and one that only crosses the boundary do not scale")
    print("# together, which is the same reason the opcode reference carries.")
    print("#")
    print("# What this catches that the gas schedule cannot: the schedule's")
    print("# digest stops a CONSTANT being changed silently, and says nothing")
    print("# about the engine drifting away from the constants. A syscall that")
    print("# doubles in cost without any constant moving is exactly the")
    print("# failure that leaves contracts underpaying, and it is invisible")
    print("# to every other check in this repository.")
    print("#")
    print("# Regenerating this file to make a failure go away hides exactly")
    print("# what it is for.")
    print()
    print(f"engine_fingerprint {ctx.get('engine_fingerprint', 'unknown')}")
    print(f"engine_source_digest "
          f"{ctx.get('engine_source_digest') or 'unknown'}")
    print(f"# generated from a {ctx.get('hogd_build_type', 'unknown')} build "
          f"on {ctx.get('hogd_host', 'unknown')} "
          f"({ctx.get('hogd_cpu_model', 'unknown')})")
    print(f"# crossing baseline {baseline:.1f} ns")
    print(f"tolerance {GUARD_TOLERANCE}")
    print()
    print("# guard <key> <cost relative to the crossing baseline>")
    for key, ratio in sorted(guards.items()):
        print(f"guard {key} {ratio:.3f}")
    return 0


def load_reference(path):
    reference = {"guards": {}, "tolerance": GUARD_TOLERANCE,
                 "engine_fingerprint": None}
    with open(path) as handle:
        for line in handle:
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if fields[0] == "guard" and len(fields) == 3:
                reference["guards"][fields[1]] = float(fields[2])
            elif fields[0] == "tolerance" and len(fields) == 2:
                reference["tolerance"] = float(fields[1])
            elif fields[0] == "engine_fingerprint" and len(fields) == 2:
                reference["engine_fingerprint"] = fields[1]
    return reference


def check_against_reference(data, reference_path):
    """Gate this run against a committed reference. Non-zero on failure."""
    reference = load_reference(reference_path)
    measured = rows(data)
    sd = stddevs(data)
    ctx = context(data)
    failures = []

    # The fingerprint first, and not as a ratio: it is exact equality over the
    # engine's own definition, so it cannot flake, and a mismatch means every
    # ratio below describes a different interpreter.
    expected = reference.get("engine_fingerprint")
    actual = ctx.get("engine_fingerprint")
    if expected and expected != "unknown" and actual != expected:
        failures.append(
            f"engine fingerprint {actual}, reference has {expected}.\n"
            "  The engine moved. Regenerating the reference is correct only "
            "once the new\n"
            "  costs have been read and the schedule changed to follow them "
            "if it needs to.")

    baseline, current = guardable(measured, sd)
    if not baseline:
        failures.append(
            "no crossing baseline in this run, so nothing could be checked.\n"
            "  The cheap stateless syscalls are what every ratio is relative "
            "to; without\n"
            "  three of them the gate cannot vouch for anything.")
    else:
        tolerance = reference["tolerance"]
        for key, guard_ratio in sorted(reference["guards"].items()):
            ratio = current.get(key)
            if ratio is None:
                # Not a warning. A guard that was not measured was not checked,
                # and a gate that passes without checking reports coverage it
                # did not provide.
                failures.append(
                    f"{key}: not measured in this run, so this guard was not "
                    "checked.\n"
                    "  Either the row failed or it reproduced too poorly to "
                    "guard; either way\n"
                    "  the gate cannot vouch for it.")
                continue
            limit = guard_ratio * (1.0 + tolerance)
            if ratio > limit:
                failures.append(
                    f"{key}: {ratio:.2f}x the crossing, was "
                    f"{guard_ratio:.2f}x (limit {limit:.2f}x).")

    print(f"checked {data.get('_path', '<run>')} against {reference_path}")
    # The converse of "a guard not measured was not checked": a row measured
    # with no guard is not checked either, and that is not a failure -- the
    # reference simply predates it. Said out loud for ML-DSA, whose rows are
    # newer than the committed reference, so a green gate is not read as
    # covering them. They are gated from the next mode=reference run on.
    unguarded = sorted(
        key for key in current
        if key.startswith(f"syscall/{MLDSA_VERIFY_FAMILY}/")
        and key not in reference["guards"])
    if unguarded:
        print(f"note: {len(unguarded)} ML-DSA-65 row(s) measured but absent "
              "from the reference, so not gated:")
        print("  " + ", ".join(unguarded))
        print("  Regenerating the reference (mode=reference) adds their "
              "guards.")
    if not failures:
        print(f"{len(reference['guards'])} guard(s) hold.")
        return 0
    print()
    for failure in failures:
        print(f"FAIL: {failure}")
    return 1


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
    parser.add_argument("--emit-reference", action="store_true",
                        help="print the gating reference for this run, to be "
                             "committed and checked by --check")
    parser.add_argument("--check", metavar="REFERENCE",
                        help="gate this run against a committed reference; "
                             "non-zero exit when a guarded cost has doubled "
                             "relative to the crossing, or when a guard could "
                             "not be measured")
    parser.add_argument("--crosscheck", metavar="OPENSSL_JSON",
                        help="openssl speed JSON from scripts/"
                             "rv32_openssl_speed.sh, run on the same host; "
                             "prints an independent check of the SHA256 and "
                             "ed25519 numbers")
    args = parser.parse_args()
    if args.resolution <= 0:
        parser.error("--resolution must be positive")

    runs = [load(path) for path in args.json]
    if args.emit_reference or args.check:
        if len(runs) != 1:
            parser.error("--emit-reference and --check take exactly one "
                         "benchmark JSON")
        if args.emit_reference and args.check:
            parser.error("--emit-reference writes a reference and --check "
                         "reads one; pick one")
        if args.emit_reference:
            return emit_reference(runs[0])
        return check_against_reference(runs[0], args.check)
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

