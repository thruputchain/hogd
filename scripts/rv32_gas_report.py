#!/usr/bin/env python3
"""Turn rv32_gas_calibrate JSON into tables, a proposed gas schedule, or a diff.

The harness measures; this script decides. Keeping the two apart matters: the
measurement is a property of the machine it ran on, while the schedule is a
protocol constant, and the rules for going from one to the other (which context
wins, how much rounding headroom, what counts as resolvable) are policy that
should be readable and arguable rather than buried in C++.

Usage:
  scripts/rv32_gas_report.py gas.json                  # per-opcode table
  scripts/rv32_gas_report.py gas.json --schedule       # proposed gas costs
  scripts/rv32_gas_report.py gas.json --workloads      # pricing-quality check
  scripts/rv32_gas_report.py gas.json --predict        # do the costs add up?
  scripts/rv32_gas_report.py before.json after.json    # compare two runs
  scripts/rv32_gas_report.py a.json b.json --schedule  # schedule both agree on
  scripts/rv32_gas_report.py gas.json --emit-reference > reference.txt
  scripts/rv32_gas_report.py gas.json --check reference.txt   # nightly gate
"""

import argparse
import json
import math
import signal
import sys

# Reading the tables through `head` or `less` is the normal way to use this, so
# a closed pipe is a normal exit, not a traceback.
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

# How many standard errors a slope must clear before it counts as a real cost
# difference from the filler. The standard error comes from the fit's own
# residuals, so it already includes the per-kernel layout bias that repeating a
# measurement cannot remove -- which is why it, and not the separately measured
# timer noise floor, is the gate.
#
# An opcode that does not clear this is not "unmeasured": it is measured, and
# the answer is that it costs what a plain ALU op costs. In an interpreter
# where dispatch dominates, that is the expected answer for most of RV32IM, and
# a schedule that invents distinctions below this line is fitting noise into a
# consensus constant.
SIGMA_THRESHOLD = 2.0

# Gas units per baseline instruction in a proposed schedule.
#
# Costs must round *up* -- rounding down sells real work for free, and the
# schedule exists to bound time from above. But rounding up at a coarse unit
# systematically over-charges, and most of RV32IM sits within a factor of two of
# the baseline, so a unit of one baseline instruction would round almost the
# whole ISA to the same 1 or 2 and throw away the distinctions this harness
# exists to find. Splitting the baseline into several units keeps the rounding
# loss bounded by 1/RESOLUTION instead of by a factor of two.
#
# The cost is that every number in the table scales by this factor, so block
# gas limits and the syscall costs in gas_cost_table.h have to scale with it or
# the two stop being comparable.
DEFAULT_RESOLUTION = 4

# How far a program may exceed the sum of its opcodes' individual worst cases
# before it counts as a bound violation rather than measurement scatter.
#
# Any excess at all is in principle a failure of the method -- worst-case
# per-opcode pricing promises exactly that a program costs no more than that sum
# -- so this margin is small. It exists only so run-to-run noise on a shared
# machine does not raise an alarm about the method.
BOUND_VIOLATION_MARGIN = 0.05

# Headroom above which a program is flagged as heavily overcharged: it is being
# billed for more than 1/(1-x) times what it costs. Not a correctness problem --
# overcharging is the safe direction -- but it is capacity the schedule gives
# away, and the outliers are where a cheaper context is worth adding.
EXTREME_HEADROOM = 0.60

# Only cells at least this many times the baseline are guarded by --check.
#
# This threshold is set from measured behaviour, not taste. Back-to-back runs on
# one host move the expensive, resolved cells by 1-7% and the near-baseline
# cells by up to 60%, because a drift far too small to matter for a 5x cell is
# an enormous relative change for a 1.05x one. A gate that guarded everything
# would fail on the near-baseline population most nights and be muted within a
# week. Guarding only cells with something real to measure is what makes a
# nightly run worth reading.
#
# That reasoning was about the *contrast* estimator, whose near-baseline cells
# were a small difference between large timings and moved by 60% run to run.
# The saturated measurement is an absolute level: three back-to-back runs put
# its relative spread at 12.5% median and 16.8% at p90 across the whole ISA,
# cheap cells included, against a tolerance of 100%. The cheap population is no
# longer the noisy one, so excluding it now discards coverage for a reason that
# has stopped being true -- on the bench host the 1.5x floor left 9 guards out
# of 100 measured cells. Guard everything at or above the baseline instead.
GUARD_MIN_RATIO = 1.0

# How far a cell has to sit from the baseline before the per-opcode table will
# call it cheaper or dearer than ADD, rather than the same. Two runs of one
# binary move the guarded population by a few percent, so a narrower band would
# make the verdict column report noise.
VERDICT_MARGIN = 0.05

# How much a guarded cell may rise before --check calls it a regression.
#
# Deliberately loose. This is a "something is wildly out of whack" alarm, not a
# performance-tracking budget: the guarded population moves by up to 7%
# back-to-back on one host and will move more across hosts and over time, and a
# periodic job that cries wolf gets muted, after which it catches nothing at
# all. At 100% a guarded cell has to *double* relative to the baseline before it
# fires -- which no amount of measurement noise will do, and which a real
# regression in a case gas already prices tightly will.
#
# Per-reference-file overridable via its `tolerance` line, if some cell ever
# needs a different one.
GUARD_TOLERANCE = 1.0


def load(path):
    with open(path) as handle:
        data = json.load(handle)
    data["_path"] = path
    return data


def provenance(data):
    return data.get("provenance") or {}


def describe_run(data):
    prov = provenance(data)
    host = prov.get("host") or "unknown host"
    cpu = prov.get("cpu_model") or "unknown cpu"
    return f"{host} ({cpu})"


def check_comparable(runs):
    """Warn when runs are not measuring the same thing.

    Two failures matter and they are different. A different engine fingerprint
    means the interpreter itself changed -- a fusion rule, a gas cost, a run
    limit -- so the numbers describe different machines-under-test and merging
    them is meaningless. A different host means the numbers are in different
    nanoseconds, which for a *schedule* is usually the point (the schedule has
    to hold on the slowest supported machine) but for a *comparison* means any
    delta is confounded by hardware.

    Neither is an error: mixing hosts deliberately is the intended workflow, and
    the harness cannot know which one you meant. But an old JSON silently
    outliving the engine it measured is the failure this whole file exists to
    prevent, so it gets said out loud.
    """
    fingerprints = {provenance(run).get("engine_fingerprint") for run in runs}
    missing = [run for run in runs if not provenance(run).get("engine_fingerprint")]
    if missing:
        print("WARNING: run(s) predate engine fingerprinting: "
              + ", ".join(run["_path"] for run in missing))
        print("         Cannot tell whether they measured the current engine.")
        print()
    elif len(fingerprints) > 1:
        print("WARNING: these runs measured DIFFERENT engines "
              f"({', '.join(sorted(f for f in fingerprints if f))}).")
        print("         A fusion rule, gas cost, or run limit changed between "
              "them, so the")
        print("         numbers are not comparable. Re-run the calibration on "
              "the current tree.")
        print()

    # The measured engine is the compiled interpreter, so a run from anything
    # but the project's Release configuration is measuring a different one --
    # -O3, -march, libc++ under clang and LTO all reach the hot path.
    nonproduction = [run for run in runs
                     if provenance(run).get("production_build") is False]
    if nonproduction:
        print("WARNING: run(s) were not built by the Release configuration: "
              + ", ".join(run["_path"] for run in nonproduction))
        print("         Those numbers describe a different interpreter than "
              "the node runs.")
        print()

    flags = {provenance(run).get("cxx_flags") for run in runs
             if provenance(run).get("cxx_flags")}
    if len(flags) > 1:
        print("WARNING: runs were built with different flags:")
        for flag in sorted(flags):
            print(f"  {flag}")
        print("         Compare them only if you meant to compare builds.")
        print()

    hosts = {describe_run(run) for run in runs}
    if len(hosts) > 1:
        print("note: runs come from different hosts:")
        for host in sorted(hosts):
            print(f"  {host}")
        print()


def context_label(cell):
    parts = [cell["context"]]
    if cell["working_set"] != "none":
        parts.append(cell["working_set"])
    if cell["magnitude"] != "default":
        parts.append(cell["magnitude"])
    if cell["branch_mode"]:
        parts.append(cell["branch_mode"])
    return "/".join(parts)


def op_width(names):
    """Width of the opcode column, wide enough for the longest name.

    Pair rows carry names like "sw;lw distinct" that overflow a column sized
    for opcodes, which runs the name straight into the context beside it.
    """
    return max(8, max((len(str(name)) for name in names), default=0) + 2)


def usable_cells(data):
    """Per-opcode cells only.

    Pair rows measure an interaction, not an instruction, so they must never
    reach the schedule: there is no opcode to charge them to. They are analysed
    separately, against the bound their two opcodes imply.
    """
    return [cell for cell in data["opcodes"]
            if cell["usable"] and not cell.get("pair")]


def pair_cells(data):
    return [cell for cell in data["opcodes"]
            if cell["usable"] and cell.get("pair")]


def cost_ns(cell):
    """This cell's per-instruction cost in nanoseconds.

    The saturated measurement when there is one: a body made entirely of this
    opcode, timed and divided by its instruction count. Absolute, so it cannot
    be negative, and it does not depend on what the filler costs.

    Falls back to the contrast estimate (baseline + slope) when a saturated
    body could not be built -- which happens only when adjacent copies of the
    opcode are a fusion rule, so no program can contain a run of them unfused.
    `saturated_note` says which. The contrast is what produced negative costs
    for about a third of sweeps, so a fallback cell is the one to distrust.
    """
    saturated = cell.get("ns_saturated", 0.0)
    if saturated > 0.0:
        return saturated
    # Second choice: the densest alternating kernel's time divided by its
    # target count. Its targets are non-adjacent, so it is available exactly
    # when saturation is not -- and it is still one absolute timing over one
    # count, so it cannot be negative either. It over-attributes the fillers'
    # time to the targets, which makes it loose by up to about 2x, but loose
    # and conservative beats precise and unstable for a gas bound.
    bound = cell.get("ns_upper_bound", 0.0)
    if bound > 0.0:
        return bound
    # Last resort: the contrast. Kept so an older run still reports, but this
    # is the estimator that produces negative costs, so a cell reaching here
    # is one to distrust.
    return cell["ns_per_instruction"]


def baseline_ns(data):
    """The cost every other cost is expressed relative to.

    The saturated ADD body, when there is one -- the same kind of measurement
    as every cell it normalizes, taken in the same run, so the two move
    together when the machine's speed does.

    The recorded `baseline` is a different kind of measurement: a fit over
    kernels of varying body size with an all-filler body. On a quiet host the
    two agree. On a loaded one they do not, and the mismatch lands in every
    ratio. Two bench runs three minutes apart recorded baselines of 1.1333 and
    0.6610 ns -- the machine really was 71% faster -- and ADDI's guarded ratio
    moved 1.68x to 4.09x purely because its kernel and the baseline's kernel
    did not scale together. Normalizing by a measurement of the same shape
    removes that.

    Falls back to the recorded baseline for runs that predate the saturated
    estimator.
    """
    for cell in data["opcodes"]:
        if is_baseline_cell(cell) and cell.get("ns_saturated", 0.0) > 0.0:
            return cell["ns_saturated"]
    return data["baseline"]["ns_per_filler_instruction"]


def is_baseline_cell(cell):
    """Is this the cell whose saturated cost every ratio is taken against?

    Its ratio to itself is 1.000 by construction, so anything that treats a
    ratio as evidence -- a guard above all -- has to know to skip it.
    """
    return (cell.get("usable") and cell["op"] == "ADD"
            and cell.get("context") == "independent")


def pair_ops(cell):
    """The two opcodes a pair row places, as the calibrator recorded them.

    The ceiling a pair's interaction has to respect is built from its OWN two
    opcodes' worst cases. Older runs predate the field; there is no safe guess
    for them, so they return None and the caller skips the bound rather than
    checking it against opcodes the pair does not contain.
    """
    ops = cell.get("pair_ops") or []
    return tuple(ops) if len(ops) == 2 else None


def measured_ops(data):
    return {cell["op"] for cell in usable_cells(data)}


def missing_ops(data, needed):
    """Opcodes a bound claim would need but this run did not measure.

    A filtered run (--op=X) measures a handful of opcodes, and the cost model
    silently falls back to the baseline for the rest. Summing that produces a
    confident-looking bound that is simply wrong -- and since a bound violation
    is reported as a failure of the method, a partial run must not be allowed to
    raise one.
    """
    return sorted(needed - measured_ops(data))


def resolved(cell):
    """Whether this cell has a cost worth using, rather than falling back to
    the baseline.

    A saturated measurement always does. It is an absolute level -- a body of
    this opcode, timed, divided by its instruction count -- so there is no null
    hypothesis to clear and nothing to be indistinguishable from. Asking it to
    prove it differs from the filler is a category error: it never touched the
    filler.

    The sigma test below belongs to the contrast, which measures the opcode
    *minus* the filler and so genuinely can fail to tell them apart. It stays
    for cells that fall back to it.

    Conflating the two silently broke the bound check. On a host where the
    contrast is noise almost nothing cleared 2 sigma, so cost_model substituted
    the baseline for nearly every opcode, the worst-case sum for a workload
    collapsed to about (instruction count x baseline), and real programs came
    out "exceeding" it -- reported as a failure of the method rather than of
    the number feeding it.
    """
    if cell.get("ns_saturated", 0.0) > 0.0:
        return True
    stderr = cell.get("slope_stderr_ns", 0.0)
    if stderr <= 0.0:
        return False
    return abs(cell["slope_ns"]) > SIGMA_THRESHOLD * stderr


def sigmas(cell):
    stderr = cell.get("slope_stderr_ns", 0.0)
    return abs(cell["slope_ns"]) / stderr if stderr > 0.0 else 0.0


def merge_runs(runs):
    """Combine several calibration runs into one set of cells.

    Per (opcode, context), the cost is the highest any run measured and the
    cell counts as resolved only if *every* run resolved it. Both halves matter
    for different reasons. Taking the max is conservative in the direction gas
    has to be conservative. Requiring unanimity is what makes a schedule
    reproducible: a cell's within-run standard error captures scatter between
    that run's kernels, but not drift between runs on a shared or thermally
    variable machine, and on such a host the near-baseline cells move by tens of
    percent between runs while the genuinely expensive ones move by one or two.
    Demanding that every run agree an opcode is more expensive than the
    baseline is what separates those two populations.

    With a single run this reduces to that run's own cells, which is why one
    run is enough to find the large effects and not enough to price the small
    ones.
    """
    merged = {}
    total_runs = len(runs)
    for data in runs:
        baseline = baseline_ns(data)
        for cell in usable_cells(data):
            key = (cell["op"], context_label(cell))
            entry = merged.get(key)
            cell_resolved = resolved(cell)
            # An unresolved cell contributes the baseline, not its fitted
            # value, so noise can never win the max and invent a cost.
            cost = cost_ns(cell) if cell_resolved else baseline
            if entry is None:
                merged[key] = {
                    "op": cell["op"],
                    "context": context_label(cell),
                    "scheduled_gas": cell["scheduled_gas"],
                    "ns_per_instruction": cost,
                    "resolved": cell_resolved,
                    "runs": 1,
                }
                continue
            entry["ns_per_instruction"] = max(entry["ns_per_instruction"], cost)
            entry["resolved"] = entry["resolved"] and cell_resolved
            entry["runs"] += 1
    # Unanimity has to mean every run, not every run that happened to contain
    # the cell. A context missing from one run -- a filtered run, or a sweep the
    # audit rejected -- would otherwise keep an earlier run's `resolved` and be
    # treated as reproduced by inputs that never saw it.
    for entry in merged.values():
        if entry["runs"] < total_runs:
            entry["resolved"] = False
    return merged


def worst_by_op(runs):
    """Per opcode, the most expensive context measured.

    Gas prices the worst case, not the average: the guest author picks the
    context, so a schedule fitted to the cheap end is a schedule an adversary
    can outrun.
    """
    worst = {}
    for entry in merge_runs(runs).values():
        current = worst.get(entry["op"])
        if (current is None
                or entry["ns_per_instruction"] > current["ns_per_instruction"]):
            worst[entry["op"]] = entry
    return worst


def print_table(data):
    check_comparable([data])
    noise_floor = data["noise_floor_ns_per_instruction"]
    baseline = baseline_ns(data)
    prov = provenance(data)
    print(f"engine: {data['engine']} (traced={data['traced']}) "
          f"fingerprint {prov.get('engine_fingerprint', 'unknown')}")
    print(f"host:   {describe_run(data)}")
    print(f"build:  {prov.get('build_type') or '(non-CMake)'} "
          f"{prov.get('cxx_flags', '')}")
    if prov.get("source_revision"):
        print(f"source: {prov['source_revision']}")
    print(
        f"baseline ADD: {baseline:.4f} ns/instr "
        f"(R2={data['baseline']['r_squared']:.4f})"
    )
    print(f"noise floor:  {noise_floor:.4f} ns/instr")
    print()
    ow = op_width(cell["op"] for cell in data["opcodes"])
    header = (f"{'op':<{ow}}{'context':<26}{'ns/instr':>10}{'contrast':>9}"
              f"{'slope':>9}{'+/-':>8}{'sigma':>7}{'gas':>5}  verdict")
    print(header)
    print("-" * len(header))
    for cell in data["opcodes"]:
        if not cell["usable"]:
            print(f"{cell['op']:<{ow}}{context_label(cell):<26}{'--':>10}"
                  f"{'--':>9}{'--':>9}{'--':>8}{'--':>7}"
                  f"{cell['scheduled_gas']:>5}"
                  f"  rejected: {cell['rejected_because']}")
            continue
        # From the cost this row actually prints, not from the contrast
        # slope. Keying the verdict off slope_ns meant a cell measured at 3x
        # baseline printed "costs less than ADD" whenever its contrast came
        # out negative -- which, on the host this work exists for, was about
        # a third of them.
        cost = cost_ns(cell)
        floor = baseline_ns(data)
        if cost > floor * (1.0 + VERDICT_MARGIN):
            verdict = "costs more than ADD"
        elif cost < floor * (1.0 - VERDICT_MARGIN):
            verdict = "costs less than ADD"
        else:
            verdict = "same as ADD"
        print(
            f"{cell['op']:<{ow}}{context_label(cell):<26}"
            f"{cost_ns(cell):>10.4f}"
            f"{cell.get('ns_per_instruction', 0.0):>9.4f}"
            f"{cell['slope_ns']:>9.4f}"
            f"{cell.get('slope_stderr_ns', 0.0):>8.4f}{sigmas(cell):>7.1f}"
            f"{cell['scheduled_gas']:>5}  {verdict}"
        )


def build_schedule(runs, resolution=DEFAULT_RESOLUTION):
    """Integer gas costs from the measured per-instruction times.

    One gas is 1/resolution of a baseline instruction, and every cost rounds up.
    The unit comes from the *smallest* baseline across the runs, which makes
    every ratio -- and so every cost -- the conservative one.
    """
    worst = worst_by_op(runs)
    if not worst:
        return None, None
    baseline = min(baseline_ns(data)
                   for data in runs)
    if baseline <= 0.0:
        return None, None
    unit = baseline / resolution
    schedule = {}
    for op, cell in sorted(worst.items()):
        # Floor every opcode at the baseline. Some opcodes measure *below* it:
        # the substitution estimator attributes the whole difference between
        # target and filler to the target, so an opcode that touches fewer
        # registers than the filler (LUI writes an immediate and reads nothing)
        # can come out with a slope steep enough to imply a per-instruction cost
        # well under one dispatch. That is not a cheaper dispatch -- every
        # instruction pays one, and the baseline sweep is what one costs -- it
        # is the model straining where target and filler differ too much for
        # the difference to belong to the target alone. Pricing there would
        # undercharge, so the floor holds.
        ns = max(cost_ns(cell), baseline)
        ratio = ns / unit
        schedule[op] = {
            "ratio": ratio,
            "proposed_gas": max(1, math.ceil(ratio - 1e-9)),
            "current_gas": cell["scheduled_gas"],
            "context": cell["context"],
            "ns": ns,
            "floored": ns > cost_ns(cell),
            "resolved": cell["resolved"],
        }
    return schedule, unit


def print_schedule(runs, resolution=DEFAULT_RESOLUTION):
    check_comparable(runs)
    schedule, unit = build_schedule(runs, resolution)
    if schedule is None:
        print("no usable cells", file=sys.stderr)
        return 1
    print(f"gas unit: {unit:.4f} ns "
          f"(1/{resolution} of a baseline instruction)")
    print(f"merged from {len(runs)} run(s), taking the highest cost each "
          f"context reached")
    print(f"baseline: {unit * resolution:.4f} ns/instruction; "
          f"no opcode prices below it")
    print('"not reproduced" means at least one run could not tell that opcode '
          "apart from the")
    print("baseline. Its cost is still the highest any run measured -- the "
          "conservative")
    print("direction -- but the distinction it draws is not one to rely on.")
    if len(runs) == 1:
        print("One run only: trust the large effects, not the near-baseline "
              "ones. Pass several")
        print("runs (ideally from several hosts) to see which distinctions "
              "survive.")
    print()
    ow = op_width(schedule)
    header = (f"{'op':<{ow}}{'worst context':<26}{'ns/instr':>10}{'ratio':>8}"
              f"{'current':>8}{'proposed':>9}  note")
    print(header)
    print("-" * len(header))
    changed = 0
    for op, entry in schedule.items():
        note = "" if entry["resolved"] else "not reproduced"
        if entry["floored"]:
            note = "measured below baseline, floored"
        if entry["proposed_gas"] != entry["current_gas"]:
            changed += 1
            note = (note + " CHANGED").strip()
        print(
            f"{op:<{ow}}{entry['context']:<26}{entry['ns']:>10.4f}"
            f"{entry['ratio']:>8.2f}{entry['current_gas']:>8}"
            f"{entry['proposed_gas']:>9}  {note}"
        )
    print()
    print(f"{changed} of {len(schedule)} opcodes would change cost")
    worst_op, worst_entry = max(
        schedule.items(),
        key=lambda item: (item[1]["proposed_gas"] - item[1]["ratio"])
        / item[1]["ratio"],
    )
    worst_rounding = (
        worst_entry["proposed_gas"] - worst_entry["ratio"]
    ) / worst_entry["ratio"]
    # Rounding up never overcharges by more than one gas unit in absolute
    # terms; as a fraction that hurts the cheapest opcode most, which is why
    # the worst case is reported by name rather than as a bound.
    print(f"worst rounding overcharge: {worst_rounding * 100:.1f}% on "
          f"{worst_op} (at most one gas unit, which is "
          f"{100.0 / resolution:.0f}% of a baseline instruction)")
    print()
    # Any run's workloads will do -- the op histogram is a property of the
    # workload, not the host -- so use the first run that recorded one.
    for run in runs:
        if any(w.get("op_counts") for w in run.get("workloads") or []):
            print_repricing(run, schedule)
            break
    print()
    print("This is a proposal, not a patch. Changing any cost in")
    print("program.h's kInstructionGasSchedule is a coordinated gas-schedule")
    print("update: it moves rv32::kGasScheduleDigest, so the pinned digest in")
    print("artifact.h and every committed RV32 artifact must be regenerated in")
    print("the same change.")
    return 0


def price_workload(workload, gas_for_op):
    """Gas one iteration of a workload would be charged under a schedule."""
    counts = workload.get("op_counts")
    if not counts:
        return None
    return sum(gas_for_op(op) * count for op, count in counts.items())


def print_repricing(data, schedule):
    """What the proposed schedule would do to the validation workloads.

    This is the question a schedule change actually has to answer. Per-opcode
    costs are inputs; the output that matters is how far apart real code's
    nanoseconds-per-gas sit, because a gas limit converts to a wall-clock bound
    at the worst of them. A change that makes some opcode's cost more accurate
    but widens this spread has made pricing worse.
    """
    workloads = data.get("workloads") or []
    if not workloads or not any(w.get("op_counts") for w in workloads):
        return

    def proposed_gas_for(op):
        entry = schedule.get(op)
        return entry["proposed_gas"] if entry else 1

    header = (f"{'workload':<16}{'ns/iter':>10}{'gas now':>9}{'ns/gas':>9}"
              f"{'gas new':>9}{'ns/gas':>9}")
    print("effect on the validation workloads:")
    print()
    print(header)
    print("-" * len(header))
    before = []
    after = []
    for workload in workloads:
        proposed = price_workload(workload, proposed_gas_for)
        if proposed is None or proposed == 0:
            continue
        new_rate = workload["ns_per_iteration"] / proposed
        before.append(workload["ns_per_gas"])
        after.append(new_rate)
        print(f"{workload['name']:<16}{workload['ns_per_iteration']:>10.2f}"
              f"{workload['scheduled_gas_per_iteration']:>9}"
              f"{workload['ns_per_gas']:>9.4f}{proposed:>9}{new_rate:>9.4f}")
    if before and after:
        old_spread = max(before) / min(before)
        new_spread = max(after) / min(after)
        print()
        print(f"ns/gas spread: {old_spread:.2f}x now -> {new_spread:.2f}x "
              f"proposed")
        verdict = ("tighter, so a gas limit gives away less real capacity"
                   if new_spread < old_spread
                   else "wider: the proposal does not improve pricing here")
        print(f"  {verdict}")
        print()
        print("Some of the remaining spread is the price of safety, not an "
              "error: workloads")
        print("whose loads are independent still pay the pointer-chase rate, "
              "because the")
        print("schedule cannot tell the two apart at charge time.")


# How well a context matches the arrangement the validation workloads use:
# independent destinations, branches falling through, memory in the low 4 KiB.
# Lower is better.
def _context_distance(cell):
    distance = 0
    if cell["context"] != "independent":
        distance += 1
    if cell["working_set"] not in ("none", "l1_4k"):
        distance += 1
    if cell["magnitude"] not in ("default", "large"):
        distance += 1
    if cell["branch_mode"] == "taken":
        distance += 1
    return distance


def cost_model(data, worst_case=False):
    """Per-opcode ns, either as the workloads arrange them or at the worst.

    Two vectors, because they answer different questions. The workload-matched
    one tests whether the per-opcode decomposition is *true*: it should predict
    a mixed program's time. The worst-case one is what the schedule prices at,
    and a workload's measured time should sit at or below it -- if it did not,
    gas would be underpricing that program.
    """
    baseline = baseline_ns(data)
    best = {}
    for cell in usable_cells(data):
        cost = cost_ns(cell) if resolved(cell) else baseline
        op = cell["op"]
        if worst_case:
            if op not in best or cost > best[op]:
                best[op] = cost
            continue
        rank = (_context_distance(cell), -cost)
        if op not in best or rank < best[op][0]:
            best[op] = (rank, cost)
    if worst_case:
        return best
    return {op: cost for op, (_, cost) in best.items()}


def taken_branch_premium(data):
    """What a taken conditional branch costs over a fall-through one.

    Every workload iteration ends in one taken branch -- the loop back edge --
    while any branch inside a body falls through. The op histogram cannot tell
    the two apart, so the premium is added once per iteration rather than
    letting a 1-in-132 instruction skew the prediction.
    """
    taken = [c for c in usable_cells(data) if c["branch_mode"] == "taken"]
    fell_through = [c for c in usable_cells(data)
                    if c["branch_mode"] == "not_taken"]
    if not taken or not fell_through:
        return 0.0
    mean_taken = sum(cost_ns(c) for c in taken) / len(taken)
    mean_through = (sum(cost_ns(c) for c in fell_through)
                    / len(fell_through))
    return max(0.0, mean_taken - mean_through)


def print_pair_bounds(data):
    """Can an interaction cost more than its opcodes' worst cases sum to?

    This is the question per-opcode worst-case pricing rests on. Each cost is
    measured for one instruction in isolation; charging a program the sum of
    those costs bounds it only if no combination is *superadditive*. A pair
    sweep measures a two-instruction interaction directly, so it can answer
    that for the interactions we know how to construct.
    """
    pairs = pair_cells(data)
    if not pairs:
        return []
    worst = cost_model(data, worst_case=True)
    baseline = baseline_ns(data)

    needed = set()
    for cell in pairs:
        ops = pair_ops(cell)
        if ops:
            needed.update(ops)
    absent = missing_ops(data, needed)
    if absent:
        print("interaction bounds: skipped -- this run did not measure "
              + ", ".join(absent))
        print("so there is no bound to compare a pair against.")
        print()
        return []

    print("interaction bounds: can a pair beat its opcodes' worst cases?")
    print()
    header = (f"{'pair':<18}{'measured':>10}{'worst sum':>11}{'headroom':>10}"
              f"{'+/-':>9}")
    print(header)
    print("-" * len(header))
    violations = []
    for cell in sorted(pairs, key=lambda c: c["op"]):
        # The bound a pair must respect is built from its OWN two opcodes'
        # worst cases. This used to read SW + LW for every pair, which was
        # right only while every pair was a store and a load; auipc;jalr
        # inherited a ceiling belonging to two opcodes it does not contain.
        ops = pair_ops(cell)
        if not ops:
            print(f"{cell['op']:<18}{'--':>10}{'--':>11}{'--':>10}"
                  f"{'--':>9}  (run predates pair_ops; no bound)")
            continue
        ceiling = sum(worst.get(op, baseline) for op in ops)
        measured = cost_ns(cell)
        headroom = (ceiling - measured) / ceiling if ceiling else 0.0
        if headroom < -BOUND_VIOLATION_MARGIN:
            violations.append((cell["op"], measured, ceiling))
        print(f"{cell['op']:<18}{measured:>10.3f}{ceiling:>11.3f}"
              f"{headroom * 100:>9.1f}%{cell.get('slope_stderr_ns', 0.0):>9.3f}")

    forward = next((c for c in pairs if "forward" in c["op"]), None)
    distinct = next((c for c in pairs if "distinct" in c["op"]), None)
    if forward and distinct:
        penalty = cost_ns(forward) - cost_ns(distinct)
        print()
        print(f"store-to-load forwarding penalty: {penalty:.3f} ns per pair")
        print(f"  ({penalty / baseline:.2f}x a baseline instruction). The two "
              f"rows differ only in")
        print("  whether the load reads the address the store just wrote, so "
              "the difference")
        print("  is the forwarding cost with instruction mix and register "
              "chain held equal.")
    print()
    return violations


def print_prediction(data):
    """Does the sum of separately measured opcode costs equal a real program?

    This is the check that the per-opcode decomposition means anything. Each
    cost was measured in isolation, by substituting one opcode into an otherwise
    fixed body; nothing so far has verified that those costs *add up* over a
    program that mixes them. Here they are summed over each validation
    workload's opcode histogram and compared against that workload's measured
    time.

    Two effects are expected, and separating them is the point:

    - A workload the lowering fuses retires the same instructions in fewer
      dispatches, so it should come in *under* the prediction. The `fused`
      column is how many body slots that applies to.
    - A workload whose operands chain (multiply_mix and divide_mix feed each
      result straight into the next add) should come in *over* a prediction
      built from independent-context costs, because that is the cheap end of
      those opcodes' range.

    So the honest additivity test is the unfused, independent workload:
    alu_mix. If the model is sound anywhere, it is sound there.
    """
    workloads = [w for w in data.get("workloads") or [] if w.get("op_counts")]
    if not workloads:
        print("no workload op histograms in this run (older schema)",
              file=sys.stderr)
        return 1

    baseline = baseline_ns(data)
    matched = cost_model(data)
    worst = cost_model(data, worst_case=True)
    premium = taken_branch_premium(data)

    def predict(workload, costs):
        return sum(costs.get(op, baseline) * count
                   for op, count in workload["op_counts"].items()) + premium

    header = (f"{'workload':<15}{'fused':>6}{'measured':>10}{'predicted':>11}"
              f"{'residual':>10}{'worst-case':>12}{'headroom':>10}")
    print("additivity: do separately measured opcode costs predict a real "
          "program?")
    print()
    print("The two percentages answer different questions. Do not read one as")
    print("the other.")
    print()
    print("residual = (measured - predicted) / predicted, against costs in the")
    print("  arrangement these workloads use. A MODEL-FIT DIAGNOSTIC, not a")
    print("  safety number: positive means the program ran slower than the sum")
    print("  of its parts, so something the per-opcode model cannot see is")
    print("  costing time. Negative means fusion or a cheap context.")
    print()
    print("headroom = (worst-case - measured) / worst-case, against the sum of")
    print("  each opcode's OWN worst measured context. THIS is the safety")
    print("  property worst-case pricing promises. Positive means the")
    print("  combination is subadditive -- charging each opcode its individual")
    print("  worst case covers the program. Negative would mean an interaction")
    print("  is SUPERADDITIVE and per-opcode worst-case pricing does not bound")
    print("  it, which would be a hole in the method, not just in a number.")
    print()
    print(header)
    print("-" * len(header))
    clean_residuals = []
    for workload in workloads:
        measured = workload["ns_per_iteration"]
        predicted = predict(workload, matched)
        ceiling = predict(workload, worst)
        residual = (measured - predicted) / predicted * 100.0 if predicted else 0.0
        headroom = (ceiling - measured) / ceiling * 100.0 if ceiling else 0.0
        if workload["fused_slots"] == 0:
            clean_residuals.append((workload["name"], residual))
        print(f"{workload['name']:<15}{workload['fused_slots']:>6}"
              f"{measured:>10.2f}{predicted:>11.2f}{residual:>9.1f}%"
              f"{ceiling:>12.2f}{headroom:>9.1f}%")
    print()
    print("predicted = sum(opcode count x its measured cost in the arrangement")
    print("these workloads use) + one taken-branch premium for the loop edge.")
    print("worst-case = the same sum at each opcode's most expensive measured")
    print("context, which is what the schedule prices at.")
    print()
    if clean_residuals:
        worst_name, worst_residual = max(clean_residuals,
                                         key=lambda item: abs(item[1]))
        mean = sum(abs(r) for _, r in clean_residuals) / len(clean_residuals)
        print(f"On the {len(clean_residuals)} workloads the lowering does not "
              f"fuse, the model is off by")
        print(f"{mean:.1f}% on average, worst {abs(worst_residual):.1f}% "
              f"({worst_name}). Those are the ones that test")
        print("additivity: a fused workload is expected to undershoot, and a "
              "chained one")
        print("(multiply_mix, divide_mix) to overshoot a prediction built from "
              "independent-")
        print("context costs -- both are the model being right about why it is "
              "wrong.")
    # A fused workload retires its instructions in fewer dispatches, so it
    # should come in under a prediction that charges for all of them. One that
    # comes in over is the interesting case: something is costing time that no
    # per-opcode cost accounts for, and the report should say so rather than
    # leave it to a careful reader of the table.
    anomalies = [w["name"] for w in workloads
                 if w["fused_slots"] > 0
                 and w["ns_per_iteration"] > predict(w, matched) * 1.05]
    if anomalies:
        print()
        print("Fused but slower than predicted: " + ", ".join(anomalies) + ".")
        print("Fusion can only remove dispatches, so a cost the per-opcode")
        print("model does not see is dominating there. For word_copy it is the")
        print("store-to-load chain: each store writes the address the next load")
        print("reads, so the pair serializes on store-to-load forwarding, which")
        print("no single-opcode sweep measures.")
        print()
        print("That is a gap in the MODEL. Whether it is a gap in the PRICING")
        print("is the headroom column, and the interaction sweep below prices")
        print("the effect directly rather than leaving it inferred.")

    print()
    needed = set()
    for workload in workloads:
        needed |= set(workload["op_counts"])
    absent = missing_ops(data, needed)
    if absent:
        print("Bound check skipped: this run did not measure "
              + ", ".join(absent) + ".")
        print("Their cost fell back to the baseline, so any bound computed "
              "from it would")
        print("be fiction. Run a full calibration to check the bound.")
        print()
        print_pair_bounds(data)
        return 0

    violations = [(w["name"], w["ns_per_iteration"], predict(w, worst))
                  for w in workloads
                  if w["ns_per_iteration"]
                  > predict(w, worst) * (1.0 + BOUND_VIOLATION_MARGIN)]
    extreme = [(w["name"], (predict(w, worst) - w["ns_per_iteration"])
                / predict(w, worst))
               for w in workloads
               if predict(w, worst)
               and (predict(w, worst) - w["ns_per_iteration"])
               / predict(w, worst) > EXTREME_HEADROOM]

    if violations:
        print("BOUND VIOLATION: these programs cost more than the sum of their")
        print("opcodes' individual worst cases:")
        for name, measured, ceiling in violations:
            print(f"  {name}: {measured:.2f} ns vs a {ceiling:.2f} ns bound "
                  f"({measured / ceiling:.2f}x)")
        print("Pricing each opcode at its own worst case does not bound these.")
        print("That is a failure of the method, not an imprecise number.")
        print("The fix keeps the schedule a flat per-instruction table: the")
        print("participating opcodes have a worse context than the sweep "
              "found, so")
        print("raise their cost, or widen the whole schedule by a margin. "
              "Charging")
        print("per-pair would make gas depend on which instructions sit next "
              "to each")
        print("other, which is a different and much harder accounting model.")
    else:
        print("No workload exceeds the sum of its opcodes' individual worst")
        print("cases, so pricing each opcode at its worst measured context "
              "bounds")
        print("every program measured here.")

    if extreme:
        print()
        print(f"Heavily overcharged (over {EXTREME_HEADROOM * 100:.0f}% "
              f"headroom -- billed for more than")
        print(f"{1 / (1 - EXTREME_HEADROOM):.1f}x what they cost):")
        for name, headroom in sorted(extreme, key=lambda item: -item[1]):
            print(f"  {name}: {headroom * 100:.0f}% headroom "
                  f"({1 / (1 - headroom):.1f}x)")
        print("Safe, but capacity given away. Each is a program whose opcodes")
        print("are being charged for a context it does not use -- which is "
              "where a")
        print("cheaper context, if one can be measured, would pay for itself.")

    print()
    pair_violations = print_pair_bounds(data)
    if pair_violations:
        print("BOUND VIOLATION on an interaction: " +
              ", ".join(name for name, _, _ in pair_violations))
        print("A pair costing more than its two opcodes' worst cases sum to "
              "means")
        print("per-opcode worst-case pricing is unsound as a method.")
    return 1 if (violations or pair_violations) else 0


def print_workloads(data):
    """Pricing quality: how far apart the ns-per-gas of real code sits.

    A gas limit converts to a wall-time bound at the worst ns_per_gas in the
    corpus, so the ratio below is the slack a block limit has to carry. It is
    the number to watch across schedule changes -- a schedule is better when
    this shrinks, regardless of what any single opcode's cost does.
    """
    workloads = data["workloads"]
    if not workloads:
        print("no workloads measured", file=sys.stderr)
        return 1
    header = f"{'workload':<16}{'ns/iter':>10}{'gas/iter':>10}{'instr':>7}{'ns/gas':>9}{'MAD':>7}{'fused':>7}"
    print(header)
    print("-" * len(header))
    for workload in workloads:
        print(
            f"{workload['name']:<16}{workload['ns_per_iteration']:>10.2f}"
            f"{workload['scheduled_gas_per_iteration']:>10}"
            f"{workload['instructions_per_iteration']:>7}"
            f"{workload['ns_per_gas']:>9.4f}{workload['relative_mad']:>7.3f}"
            f"{workload['fused_slots']:>7}"
        )
    rates = [w["ns_per_gas"] for w in workloads if w["ns_per_gas"] > 0]
    if rates:
        print()
        print(f"ns/gas range: {min(rates):.4f} .. {max(rates):.4f}")
        print(f"spread (max/min): {max(rates) / min(rates):.2f}x")
        print()
        print("A block gas limit has to be sized for the worst ns/gas here, so")
        print("this spread is how much real capacity the schedule gives away.")
    return 0


def print_comparison(before, after):
    check_comparable([before, after])
    before_cells = {(c["op"], context_label(c)): c for c in usable_cells(before)}
    after_cells = {(c["op"], context_label(c)): c for c in usable_cells(after)}
    floor = max(
        before["noise_floor_ns_per_instruction"],
        after["noise_floor_ns_per_instruction"],
    )
    print(f"baseline ADD: {before['baseline']['ns_per_filler_instruction']:.4f}"
          f" -> {after['baseline']['ns_per_filler_instruction']:.4f} ns")
    print(f"noise floor (max of the two runs): {floor:.4f} ns/instr")
    print()
    ow = op_width(key[0] for key in before_cells.keys() & after_cells.keys())
    header = (f"{'op':<{ow}}{'context':<26}{'before':>10}{'after':>10}"
              f"{'delta':>10}{'%':>8}")
    print(header)
    print("-" * len(header))
    for key in sorted(before_cells.keys() & after_cells.keys()):
        old = cost_ns(before_cells[key])
        new = cost_ns(after_cells[key])
        delta = new - old
        # Anything inside the noise floor is not a change worth printing a
        # percentage for.
        if abs(delta) <= floor:
            continue
        percent = (delta / old * 100.0) if old else 0.0
        print(f"{key[0]:<{ow}}{key[1]:<26}{old:>10.4f}{new:>10.4f}"
              f"{delta:>+10.4f}{percent:>+8.1f}")
    return 0


def relative_cost(cell, baseline):
    """Cost as a multiple of the baseline instruction.

    Ratios rather than nanoseconds is what makes a reference file portable. An
    absolute cost is a fact about one machine and expires the moment the
    nightly job lands on a different runner; "a dependent load costs 5.9 times
    what a plain ALU op costs on the same machine" is a fact about the engine,
    and holds across hosts well enough to gate on.
    """
    return cost_ns(cell) / baseline if baseline > 0 else 0.0


def emit_reference(data):
    """Write the reference a later run is checked against."""
    prov = provenance(data)
    baseline = baseline_ns(data)
    print("# RV32 gas calibration reference.")
    print("#")
    print("# Regenerate with:")
    print("#   scripts/rv32_gas_calibrate.sh BUILD_DIR --out /tmp/gas.json")
    print("#   scripts/rv32_gas_report.py /tmp/gas.json --emit-reference \\")
    print("#     > hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt")
    print("#")
    print("# Or run the RV32 Gas Calibration workflow with mode=reference,")
    print("# which prints this block from the bench host.")
    print("#")
    print("# Checked by scripts/rv32_gas_check.sh. Costs are multiples of the")
    print("# baseline instruction measured in the same run, not nanoseconds.")
    print("#")
    print("# That makes the file portable across IDENTICAL hardware, and not")
    print("# across microarchitectures. Measured: a reference taken on one")
    print("# cloud host failed 3 of 18 guards on another with the same engine")
    print("# fingerprint. The baseline is dispatch-bound and scales with core")
    print("# width and clock; a chained multiply or a pointer-chased load is")
    print("# latency-bound and does not, so their ratio is not invariant.")
    print("# Regenerate on the machine class the check will run on -- the")
    print("# calibration workflow's mode=reference does exactly that.")
    print("#")
    print("# Only cells at or above")
    print(f"# {GUARD_MIN_RATIO}x baseline are guarded: a cell cheaper than")
    print("# the baseline instruction has no headroom worth watching.")
    print("#")
    print("# The tolerance is loose on purpose: this is a \"something is")
    print("# wildly out of whack\" alarm, not a performance budget. A guarded")
    print("# cell has to double relative to the baseline before it fires.")
    print("#")
    print("# Regenerating this file to make a failure go away hides exactly")
    print("# what it is for.")
    print()
    print(f"engine_fingerprint {prov.get('engine_fingerprint', 'unknown')}")
    print(f"engine_source_digest "
          f"{prov.get('engine_source_digest') or 'unknown'}")
    print(f"# generated from a {prov.get('build_type', 'unknown')} build on "
          f"{describe_run(data)}")
    print(f"tolerance {GUARD_TOLERANCE}")
    print()
    print("# guard <op> <context> <cost relative to baseline> <current gas>")
    for cell in sorted(usable_cells(data),
                       key=lambda c: (c["op"], context_label(c))):
        ratio = relative_cost(cell, baseline)
        if ratio < GUARD_MIN_RATIO or not resolved(cell):
            continue
        # The cell that *defines* the baseline divides by itself, so its ratio
        # is 1.000 on every host and every run and the guard can never fire.
        # Emitting it reports coverage the gate structurally cannot provide,
        # and it was counted in the "guards: N" line offered as evidence of it.
        if is_baseline_cell(cell):
            continue
        print(f"guard {cell['op']} {context_label(cell)} {ratio:.3f} "
              f"{cell['scheduled_gas']}")
    rates = [w["ns_per_gas"] for w in data.get("workloads") or []
             if w["ns_per_gas"] > 0]
    if rates:
        print()
        print("# Worst-over-best ns/gas across the validation workloads: how")
        print("# much real capacity the current schedule gives away. Rising")
        print("# means pricing got less even.")
        print(f"workload_spread {max(rates) / min(rates):.3f}")
    return 0


def load_reference(path):
    reference = {"guards": {}, "fingerprint": None, "source_digest": None,
                 "spread": None, "tolerance": GUARD_TOLERANCE}
    with open(path) as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if fields[0] == "engine_fingerprint" and len(fields) == 2:
                reference["fingerprint"] = fields[1]
            elif fields[0] == "engine_source_digest" and len(fields) == 2:
                reference["source_digest"] = fields[1]
            elif fields[0] == "tolerance" and len(fields) == 2:
                reference["tolerance"] = float(fields[1])
            elif fields[0] == "workload_spread" and len(fields) == 2:
                reference["spread"] = float(fields[1])
            elif fields[0] == "guard" and len(fields) == 5:
                reference["guards"][(fields[1], fields[2])] = {
                    "ratio": float(fields[3]),
                    "gas": int(fields[4]),
                }
    return reference


def check_against_reference(data, reference_path):
    """Nightly gate: has the calibration stopped describing the engine?

    Two independent failures, deliberately reported separately because they
    need different responses.

    The engine fingerprint is a pure equality check with no timing in it, so it
    cannot flake, and it catches the most likely way a calibration goes stale:
    someone changes a fusion rule or a gas cost and the numbers quietly keep
    describing the old interpreter. Nothing about stale numbers looks wrong,
    which is why this check has to be mechanical.

    The guarded ratios are timed, and catch the engine getting slower at
    something relative to its own baseline -- a regression in a case gas
    already prices tightly.
    """
    reference = load_reference(reference_path)
    prov = provenance(data)
    baseline = baseline_ns(data)
    failures = []
    warnings = []

    fingerprint = prov.get("engine_fingerprint")
    if reference["fingerprint"] and fingerprint != reference["fingerprint"]:
        failures.append(
            f"engine changed: fingerprint {fingerprint} != reference "
            f"{reference['fingerprint']}.\n"
            "  The lowering's fusion behaviour, gas schedule, or run limits "
            "moved, so the\n"
            "  calibration behind the current gas costs describes a different "
            "interpreter.\n"
            "  Re-run the calibration and review the costs before "
            "regenerating the reference."
        )

    # A changed source digest warns rather than fails, and the split is
    # deliberate. A changed behavioural fingerprint means the lowering's
    # decisions moved -- fusion, gas, limits -- so the old numbers are
    # definitely about a different engine. A changed source digest only means
    # the interpreter was edited: the numbers may be fine, or may not. Failing
    # on every edit would leave the periodic job red through normal development
    # until somebody regenerated the reference, and a job that is always red is
    # a job nobody reads.
    source_digest = prov.get("engine_source_digest")
    if (reference["source_digest"] and source_digest
            and source_digest != reference["source_digest"]):
        warnings.append(
            f"interpreter source changed since the reference "
            f"({reference['source_digest']} -> {source_digest}). Fusion, gas "
            "and limits are\n"
            "  unchanged, so the guards below still apply, but a handler or "
            "dispatch edit\n"
            "  can move costs without moving them. Review and regenerate if "
            "they did."
        )

    measured = {}
    for cell in usable_cells(data):
        measured[(cell["op"], context_label(cell))] = cell

    tolerance = reference["tolerance"]
    for key, guard in sorted(reference["guards"].items()):
        cell = measured.get(key)
        if cell is None:
            # Not a warning. A guard that was not measured was not checked, and
            # a gate that passes without checking its guards is worse than no
            # gate: it reports coverage it did not provide. A partial run
            # (--op=X) or a sweep the audit rejected both land here.
            failures.append(
                f"{key[0]} {key[1]}: not measured in this run, so this guard "
                "was not checked.\n"
                "  Either the run was filtered (--op=...) or the sweep was "
                "rejected; either way\n"
                "  the gate cannot vouch for this cost."
            )
            continue
        ratio = relative_cost(cell, baseline)
        limit = guard["ratio"] * (1.0 + tolerance)
        if ratio > limit:
            failures.append(
                f"{key[0]} {key[1]}: {ratio:.2f}x baseline, was "
                f"{guard['ratio']:.2f}x (limit {limit:.2f}x)"
            )

    # A cell that has become expensive but is not in the reference is new, and
    # a newly expensive instruction is exactly the thing a gas schedule needs to
    # hear about. It warns rather than fails: it may simply be the first run on
    # a host where that context is slower.
    for key, cell in sorted(measured.items()):
        if key in reference["guards"] or not resolved(cell):
            continue
        ratio = relative_cost(cell, baseline)
        if ratio >= GUARD_MIN_RATIO * (1.0 + tolerance):
            warnings.append(
                f"{key[0]} {key[1]}: {ratio:.2f}x baseline and not in the "
                f"reference (charged {cell['scheduled_gas']} gas)"
            )

    # A program costing more than its opcodes' individual worst cases sum to
    # means per-opcode worst-case pricing does not bound it. That is a failure
    # of the method rather than a drifted number, so it fails the check
    # regardless of what the reference says.
    pair_floor = baseline_ns(data)
    worst_costs = cost_model(data, worst_case=True)
    for cell in pair_cells(data):
        ops = pair_ops(cell)
        if not ops or missing_ops(data, set(ops)):
            continue
        ceiling = sum(worst_costs.get(op, pair_floor) for op in ops)
        if cost_ns(cell) > ceiling * (1.0 + BOUND_VIOLATION_MARGIN):
            failures.append(
                f"{cell['op']}: {cost_ns(cell):.2f} ns exceeds "
                f"the {ceiling:.2f} ns bound its two opcodes "
                f"({' + '.join(ops)}) worst cases imply.\n"
                "  Pricing each opcode at its own worst case does not bound "
                "programs that\n"
                "  use them together. Raise the participating opcodes' cost "
                "or widen the\n"
                "  schedule by a margin -- both keep the flat per-instruction "
                "table."
            )

    rates = [w["ns_per_gas"] for w in data.get("workloads") or []
             if w["ns_per_gas"] > 0]
    if rates and reference["spread"]:
        spread = max(rates) / min(rates)
        limit = reference["spread"] * (1.0 + tolerance)
        if spread > limit:
            failures.append(
                f"workload ns/gas spread: {spread:.2f}x, was "
                f"{reference['spread']:.2f}x (limit {limit:.2f}x)"
            )

    print(f"checked {data['_path']} against {reference_path}")
    print(f"  host:        {describe_run(data)}")
    print(f"  source:      {source_digest or 'unknown'}")
    print(f"  fingerprint: {fingerprint} "
          f"({'matches' if fingerprint == reference['fingerprint'] else 'DIFFERS FROM'}"
          f" reference)")
    print(f"  guards:      {len(reference['guards'])}")
    print()
    for warning in warnings:
        print(f"WARN  {warning}")
    if warnings:
        print()
    if not failures:
        print("PASS: the calibration still describes this engine.")
        return 0
    for failure in failures:
        print(f"FAIL  {failure}")
    print()
    print(f"{len(failures)} check(s) failed.")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("json", nargs="+", help="calibration JSON (one, or two to compare)")
    parser.add_argument("--schedule", action="store_true",
                        help="print a proposed integer gas schedule")
    parser.add_argument("--workloads", action="store_true",
                        help="print the validation workload pricing table")
    parser.add_argument("--predict", action="store_true",
                        help="check that summed per-opcode costs predict the "
                             "validation workloads")
    parser.add_argument("--emit-reference", action="store_true",
                        help="write a reference file for --check")
    parser.add_argument("--check", metavar="REFERENCE",
                        help="compare a run against a reference file and exit "
                             "non-zero on regression")
    parser.add_argument("--resolution", type=int, default=DEFAULT_RESOLUTION,
                        help="gas units per baseline instruction "
                             f"(default {DEFAULT_RESOLUTION})")
    args = parser.parse_args()

    runs = [load(path) for path in args.json]
    if args.emit_reference:
        if len(runs) != 1:
            parser.error("--emit-reference takes exactly one JSON file")
        return emit_reference(runs[0])
    if args.check:
        if len(runs) != 1:
            parser.error("--check takes exactly one JSON file")
        return check_against_reference(runs[0], args.check)
    if args.schedule:
        return print_schedule(runs, args.resolution)
    if len(args.json) == 2:
        return print_comparison(runs[0], runs[1])
    if len(args.json) != 1:
        parser.error("pass one JSON file, two to compare, "
                     "or several with --schedule")
    if args.predict:
        return print_prediction(runs[0])
    if args.workloads:
        return print_workloads(runs[0])
    print_table(runs[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
