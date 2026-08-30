// RV32 gas calibration harness.
//
// Measures what the production engine (ThreadedMachine, tracing off) actually
// costs per instruction, so the gas schedule in program.h can be set from
// measurement instead of guessed. See README.md for the method and for how to
// read the output.
//
// The estimator is a controlled contrast, not a regression over workloads.
// Every kernel in a sweep has the same body slot count, the same code size, the
// same trip count, the same register plan, and the same body boundaries; the
// only difference is how many body slots hold the target instruction instead of
// the filler. Fitting time against that count gives ns-per-instruction directly
// as a slope, with all fixed overhead in the intercept. There is no design
// matrix to condition and nothing for the opcodes to be collinear with, which
// is the failure mode of fitting a cost table to a corpus of mixed programs:
// in real code opcode counts move together, so the fit is free to trade one
// opcode's cost against another's and lands on coefficients that fit the
// corpus without meaning anything per-opcode.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace hogd::rv32::gas_calibration {
namespace {

struct Options {
  // A wide body is the cheapest precision there is. The slope's standard error
  // falls with the spread of the target count, so doubling the body halves the
  // uncertainty at the same wall time -- far better value than more repeats,
  // which only average timer jitter and leave the per-kernel layout bias that
  // actually dominates the residual.
  uint32_t body_slots = 512;
  uint32_t repeats = 9;
  uint32_t warmup_runs = 2;
  double target_ns = 2'000'000.0; // 2 ms per run
  std::string out_path;
  bool quick = false;
  std::string only_op;
  // Escape hatch for working on the harness itself, never for a calibration
  // whose numbers anyone will act on.
  bool allow_nonproduction = false;
  // Distinct target placements measured at each target count. Two phases do
  // identical work, so spreading each point over several of them averages out
  // per-kernel layout bias instead of baking one kernel's luck into the slope.
  uint32_t placements = 5;
};

// One measured (op, context) cell.
struct SweepResult {
  std::string op;
  std::string context;
  std::string working_set;
  std::string magnitude;
  std::string branch_mode;
  // A pair row measures an interaction, not an opcode, so it must never feed
  // the per-opcode schedule -- only the superadditivity analysis.
  bool is_pair = false;
  // For a pair row, the two opcodes it places, so a consumer can build the
  // ceiling this interaction has to respect out of the right worst cases.
  // Empty for a single-opcode row.
  std::vector<std::string> pair_ops;
  uint64_t scheduled_gas = 0;
  // Absolute upper bound on one target's cost, from the densest kernel alone.
  // Non-negative by construction; see where it is computed for the derivation.
  double ns_upper_bound = 0.0;
  // Absolute ns per instruction of a body made entirely of this target. The
  // primary number: no contrast, no filler, so it cannot come out negative.
  // Zero when the saturated body could not be measured (see the note).
  double ns_saturated = 0.0;
  std::string saturated_note;
  LinearFit fit;
  double worst_relative_mad = 0.0;
  LoweringAudit audit;
  size_t measured_points = 0;
  bool usable = false;
  std::string rejected_because;
  // Absolute ns for one instance, once the filler's own absolute cost is added
  // back to the slope.
  double ns_per_instruction = 0.0;
};

std::string
escape_json(std::string_view text)
{
  std::string out;
  for (const char character : text) {
    if (character == '"' || character == '\\') {
      out.push_back('\\');
    }
    out.push_back(character);
  }
  return out;
}

std::string
json_string_array(std::span<const std::string> items)
{
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += '"';
    out += escape_json(items[i]);
    out += '"';
    if (i + 1 < items.size()) {
      out += ", ";
    }
  }
  out += ']';
  return out;
}

// Kernels for one sweep, kept alive together: the pre-instantiations borrow
// nothing from the Kernel, but the Kernel carries the audit inputs and the
// scheduled gas the reporter needs.
struct SweepKernels {
  std::vector<Kernel> kernels;
  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
  std::vector<uint32_t> target_counts;
};

std::expected<SweepKernels, KernelError>
build_sweep(const KernelSpec &base, std::span<const uint32_t> target_counts,
            uint32_t trip_count, uint32_t placements)
{
  SweepKernels sweep;
  // Placements already emitted for the current target count. Phases are meant
  // to average layout bias, which they only do if they actually produce
  // different layouts -- and at the densest point the spacing can be small
  // enough that several phases coincide. Rather than assume distinctness,
  // check it: a repeated layout adds no information and would weight one
  // kernel's luck several times in the fit.
  std::vector<std::vector<uint32_t>> seen_placements;
  for (const uint32_t count : target_counts) {
    seen_placements.clear();
    for (uint32_t phase = 0; phase < placements; ++phase) {
      KernelSpec spec = base;
      spec.target_slots = count;
      spec.trip_count = trip_count;
      // Phase only rotates placement, so a zero-target kernel has nothing to
      // rotate; measuring it once avoids three identical points dominating the
      // fit's low end.
      if (count == 0 && phase > 0) {
        continue;
      }
      spec.placement_phase = phase;
      auto kernel = build_kernel(spec);
      if (!kernel.has_value()) {
        return std::unexpected(kernel.error());
      }
      if (std::ranges::find(seen_placements, kernel->target_slot_pcs) !=
          seen_placements.end()) {
        continue;
      }
      seen_placements.push_back(kernel->target_slot_pcs);
      auto program =
          ThreadedMachinePreInstantiation::create(kernel->code,
                                                  kernel->initial_memory, 0,
                                                  kernel->memory_bytes,
                                                  /*trace=*/false);
      if (!program.has_value()) {
        return std::unexpected(KernelError::kUndecodable);
      }
      sweep.kernels.push_back(std::move(*kernel));
      sweep.programs.push_back(
          std::make_unique<ThreadedMachinePreInstantiation>(
              std::move(*program)));
      sweep.target_counts.push_back(count);
    }
  }
  return sweep;
}

// Target-slot counts for an opcode sweep. Capped at half the interior so no two
// targets are ever adjacent, which is what keeps every target's neighbours
// filler and therefore keeps fusion out of the body.
std::vector<uint32_t>
sweep_counts(uint32_t body_slots, bool quick, const KernelSpec &spec)
{
  const uint32_t cap = max_target_slots(body_slots, spec);
  if (quick) {
    return {0, cap / 2, cap};
  }
  return {0, cap / 5, (2 * cap) / 5, (3 * cap) / 5, (4 * cap) / 5, cap};
}

SweepResult
run_sweep(const KernelSpec &base, const Options &options, double filler_ns,
          const std::string &branch_mode)
{
  SweepResult result;
  result.is_pair = base.pair != PairKind::kNone;
  result.op = result.is_pair ? pair_name(base.pair) : op_name(base.target);
  result.context = result.is_pair ? "pair" : context_name(base.context);
  result.working_set = working_set_name(base.working_set);
  result.magnitude = magnitude_name(base.magnitude);
  result.branch_mode = branch_mode;
  // What the current schedule charges for one substitution: both halves of a
  // pair, or the single opcode. The halves come from pair_components rather
  // than a literal SW+LW, which was correct only while every pair happened to
  // be a store and a load.
  if (result.is_pair) {
    const auto [first, second] = pair_components(base.pair);
    result.pair_ops = {op_name(first), op_name(second)};
    result.scheduled_gas = scheduled_gas_for(first) + scheduled_gas_for(second);
  }
  else {
    result.scheduled_gas = scheduled_gas_for(base.target);
  }

  const std::vector<uint32_t> counts =
      sweep_counts(options.body_slots, options.quick, base);
  auto sweep = build_sweep(base, counts, 64, options.placements);
  if (!sweep.has_value()) {
    result.rejected_because =
        "kernel not representable: " + kernel_error_name(sweep.error());
    return result;
  }

  const uint32_t trip_count =
      tune_trip_count(*sweep->programs.back(), 64, options.target_ns);
  sweep = build_sweep(base, counts, trip_count, options.placements);
  if (!sweep.has_value()) {
    result.rejected_because = "kernel not representable at the tuned trip "
                              "count: " +
                              kernel_error_name(sweep.error());
    return result;
  }

  // Audit every kernel in the sweep, not just the densest. A fit is a
  // statement about the difference between these specific programs, so a
  // fusion or run split in any one of them corrupts the slope -- and the
  // handler lookups are cached, so checking all of them costs little.
  //
  // After the rebuild, and at the trip count the measured kernels were built
  // with. This used to run before the sweep was rebuilt at the tuned trip
  // count, so the verdict described programs that were then discarded and the
  // ones actually timed went unchecked.
  KernelSpec audited = base;
  audited.trip_count = trip_count;
  for (size_t i = 0; i < sweep->kernels.size(); ++i) {
    audited.target_slots = sweep->target_counts[i];
    const LoweringAudit audit = audit_kernel(sweep->kernels[i], audited);
    if (i == sweep->kernels.size() - 1) {
      result.audit = audit;
    }
    // Check the lowering against intent. For almost every sweep that means no
    // slot was rewritten; for a pair chosen because the engine fuses it, it
    // means the opposite, and a sweep that failed to fuse would be measuring
    // the unfused sequence while claiming to measure the fused one.
    const bool expects_fusion = pair_expects_fusion(base.pair);
    const bool matched = expects_fusion
                             ? audit.rewritten_slots >= sweep->target_counts[i]
                             : audit.clean();
    if (!matched && sweep->target_counts[i] > 0) {
      result.audit = audit;
      result.rejected_because =
          expects_fusion
              ? "lowering fused only " + std::to_string(audit.rewritten_slots) +
                    " of " + std::to_string(sweep->target_counts[i]) +
                    " pairs; this sweep exists to measure the fused form"
              : "lowering rewrote " + std::to_string(audit.rewritten_slots) +
                    " body slot(s) of the " +
                    std::to_string(sweep->target_counts[i]) +
                    "-target kernel (first at " +
                    std::to_string(audit.first_rewritten_slot) + ")";
      return result;
    }
  }

  std::vector<MeasurementPoint> points;
  points.reserve(sweep->programs.size());
  for (size_t i = 0; i < sweep->programs.size(); ++i) {
    points.push_back(
        {.label = result.op + "/" + std::to_string(sweep->target_counts[i]),
         .x = static_cast<double>(sweep->target_counts[i]),
         .program = sweep->programs[i].get(),
         .trip_count = trip_count});
  }
  measure_interleaved(points, {.repeats = options.repeats,
                               .warmup_runs = options.warmup_runs});

  for (const MeasurementPoint &point : points) {
    if (!point.ran_clean) {
      result.rejected_because = "kernel did not halt cleanly";
      return result;
    }
    result.worst_relative_mad =
        std::max(result.worst_relative_mad, point.relative_mad());
  }
  result.fit = fit_line(points);
  result.measured_points = points.size();
  // The slope is the cost of a target *minus* the filler it displaced; adding
  // the filler's own measured absolute cost back gives an absolute per
  // instruction number.
  result.ns_per_instruction = filler_ns + result.fit.slope;

  // An absolute upper bound, which is what a gas schedule actually needs.
  //
  // The slope above is a contrast: a small difference between large timings,
  // fitted across kernels that differ in layout as well as in content. On a
  // machine where that layout bias is comparable to the effect, the fit hands
  // the bias to the slope, and the result can come out *negative* -- an
  // instruction that costs less than nothing, which is not a number a gas
  // schedule can use. Measured on a 64-core EPYC: half the ISA priced below a
  // no-op, and the same sweeps moved by whole dispatches between runs.
  //
  // The densest kernel needs no contrast. It runs `target_slots` targets and
  // some fillers per iteration, so
  //
  //     T = n_target * c_target + n_filler * c_filler + loop overhead
  //
  // and since every one of those terms is non-negative, T / n_target is an
  // upper bound on c_target unconditionally -- no subtraction, no fit, no
  // dependence on what the filler costs. It cannot be negative.
  //
  // The looseness is 1 + (n_filler * c_filler) / (n_target * c_target), so
  // with the body half targets it is at most ~2x, and it *tightens as the
  // target gets more expensive*: a pointer-chased load at 6 ns against a
  // 0.65 ns filler is bounded to within 11%. The margin is worst exactly
  // where the opcode is cheap and being wrong costs least.
  const uint32_t densest = sweep->target_counts.back();
  if (densest > 0) {
    std::vector<double> dense_samples;
    for (size_t i = 0; i < points.size(); ++i) {
      if (sweep->target_counts[i] == densest) {
        dense_samples.push_back(points[i].median());
      }
    }
    if (!dense_samples.empty()) {
      // Median over placement phases: the phases do identical work, so the
      // spread between them is layout systematics. That spread is small next
      // to the structural margin above, so there is nothing to gain by taking
      // the worst one and stability to lose.
      std::ranges::sort(dense_samples);
      const double dense_ns = dense_samples[dense_samples.size() / 2];
      result.ns_upper_bound = dense_ns / static_cast<double>(densest);
    }
  }
  // The primary measurement: a body made entirely of this target.
  //
  // Everything above is a contrast -- the target's cost minus the filler it
  // displaced -- and a contrast between two large timings goes negative as
  // soon as layout noise rivals the effect. Roughly 30% of sweeps came out
  // negative that way, which is not a cost a gas schedule can use.
  //
  // A saturated body has no filler to subtract. Its time divided by its
  // instruction count is, directly, what a program made of this instruction
  // costs per instruction -- which is also the question worst-case pricing
  // asks. Only ADDI declines to be measured this way, because addi;addi is a
  // fusion rule; the audit catches that rather than letting the fused cost be
  // reported as the unfused one.
  {
    KernelSpec sat = base;
    sat.saturated = true;
    sat.body_slots = options.body_slots;
    sat.target_slots = max_target_slots(options.body_slots, sat);
    sat.placement_phase = 0;
    sat.trip_count = 64;
    auto probe = build_kernel(sat);
    if (!probe.has_value()) {
      result.saturated_note = "saturated kernel not representable: " +
                              kernel_error_name(probe.error());
    }
    else {
      auto seed =
          ThreadedMachinePreInstantiation::create(probe->code,
                                                  probe->initial_memory, 0,
                                                  probe->memory_bytes,
                                                  /*trace=*/false);
      if (!seed.has_value()) {
        result.saturated_note = "saturated probe would not instantiate";
        result.usable = true;
        return result;
      }
      sat.trip_count = tune_trip_count(*seed, 64, options.target_ns);
      auto kernel = build_kernel(sat);
      if (!kernel.has_value()) {
        result.saturated_note = "saturated kernel not representable at the "
                                "tuned trip count: " +
                                kernel_error_name(kernel.error());
      }
      else {
        const LoweringAudit audit = audit_kernel(*kernel, sat);
        if (!audit.clean()) {
          result.saturated_note =
              "saturated body fused " + std::to_string(audit.rewritten_slots) +
              " slot(s) (first at " +
              std::to_string(audit.first_rewritten_slot) +
              "); adjacent copies of this opcode are a fusion rule, so a "
              "program cannot be made of it unfused";
        }
        else {
          auto program =
              ThreadedMachinePreInstantiation::create(kernel->code,
                                                      kernel->initial_memory, 0,
                                                      kernel->memory_bytes,
                                                      false);
          if (!program.has_value()) {
            result.saturated_note = "saturated kernel would not instantiate";
          }
          else {
            std::vector<MeasurementPoint> one{
                {.label = result.op + "/saturated",
                 .x = 0.0,
                 .program = &*program,
                 .trip_count = sat.trip_count}};
            measure_interleaved(one, {.repeats = options.repeats,
                                      .warmup_runs = options.warmup_runs});
            if (!one[0].ran_clean) {
              result.saturated_note = "saturated kernel did not halt cleanly";
            }
            else {
              result.ns_saturated =
                  one[0].median() /
                  static_cast<double>(kernel->instructions_per_iteration);
            }
          }
        }
      }
    }
  }

  // A cell with no saturated measurement and a negative contrast has no cost a
  // schedule could use. Report that rather than passing a negative number
  // downstream: worst-case pricing would sort it to the bottom and quietly
  // treat the opcode as free.
  if (result.ns_saturated <= 0.0 && result.ns_per_instruction <= 0.0) {
    result.rejected_because =
        "no usable cost: " +
        (result.saturated_note.empty() ? std::string("no saturated body")
                                       : result.saturated_note) +
        ", and the contrast fallback measured " +
        std::format("{:.4f}", result.ns_per_instruction) + " ns";
    return result;
  }

  result.usable = true;
  return result;
}

// Absolute ns per filler instruction, from a sweep that varies the body slot
// count with an all-filler body. This is the anchor every opcode slope is
// added to; without it the harness could only report costs relative to ADD.
struct BaselineResult {
  double ns_per_filler = 0.0;
  LinearFit fit;
  double worst_relative_mad = 0.0;
  bool usable = false;
};

BaselineResult
run_baseline(const Options &options)
{
  BaselineResult baseline;
  const std::vector<uint32_t> body_sizes =
      options.quick ? std::vector<uint32_t>{64, 160, 256}
                    : std::vector<uint32_t>{64, 112, 160, 208, 256};

  std::vector<Kernel> kernels;
  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
  for (const uint32_t slots : body_sizes) {
    KernelSpec spec;
    spec.target = Op::kAdd;
    spec.target_slots = 0; // all filler
    spec.body_slots = slots;
    spec.trip_count = 64;
    auto kernel = build_kernel(spec);
    if (!kernel.has_value()) {
      return baseline;
    }
    auto program =
        ThreadedMachinePreInstantiation::create(kernel->code,
                                                kernel->initial_memory, 0,
                                                kernel->memory_bytes,
                                                /*trace=*/false);
    if (!program.has_value()) {
      return baseline;
    }
    kernels.push_back(std::move(*kernel));
    programs.push_back(
        std::make_unique<ThreadedMachinePreInstantiation>(std::move(*program)));
  }

  const uint32_t trip_count =
      tune_trip_count(*programs.back(), 64, options.target_ns);
  std::vector<MeasurementPoint> points;
  points.reserve(programs.size());
  for (size_t i = 0; i < programs.size(); ++i) {
    points.push_back({.label = "baseline/" + std::to_string(body_sizes[i]),
                      .x = static_cast<double>(body_sizes[i]),
                      .program = programs[i].get(),
                      .trip_count = trip_count});
  }
  // Re-tune: the trip count above was tuned on the seed kernels, and the
  // measured kernels must use the tuned count to hit the target duration.
  for (size_t i = 0; i < body_sizes.size(); ++i) {
    KernelSpec spec;
    spec.target = Op::kAdd;
    spec.target_slots = 0;
    spec.body_slots = body_sizes[i];
    spec.trip_count = trip_count;
    auto kernel = build_kernel(spec);
    if (!kernel.has_value()) {
      return baseline;
    }
    auto program =
        ThreadedMachinePreInstantiation::create(kernel->code,
                                                kernel->initial_memory, 0,
                                                kernel->memory_bytes,
                                                /*trace=*/false);
    if (!program.has_value()) {
      return baseline;
    }
    kernels[i] = std::move(*kernel);
    programs[i] =
        std::make_unique<ThreadedMachinePreInstantiation>(std::move(*program));
    points[i].program = programs[i].get();
    points[i].trip_count = trip_count;
  }

  measure_interleaved(points, {.repeats = options.repeats,
                               .warmup_runs = options.warmup_runs});
  for (const MeasurementPoint &point : points) {
    if (!point.ran_clean) {
      return baseline;
    }
    baseline.worst_relative_mad =
        std::max(baseline.worst_relative_mad, point.relative_mad());
  }
  baseline.fit = fit_line(points);
  baseline.ns_per_filler = baseline.fit.slope;
  baseline.usable = baseline.ns_per_filler > 0.0;
  return baseline;
}

// Two identical all-filler kernels measured under different labels inside one
// interleaved schedule. Their medians should agree; whatever they differ by is
// the floor below which this host cannot resolve a cost difference at all.
// Every slope smaller than this is noise wearing a number.
double
measure_noise_floor(const Options &options)
{
  KernelSpec spec;
  spec.target = Op::kAdd;
  spec.target_slots = 0;
  spec.body_slots = options.body_slots;
  spec.trip_count = 64;
  auto kernel = build_kernel(spec);
  if (!kernel.has_value()) {
    return 0.0;
  }
  auto seed = ThreadedMachinePreInstantiation::create(kernel->code,
                                                      kernel->initial_memory, 0,
                                                      kernel->memory_bytes,
                                                      /*trace=*/false);
  if (!seed.has_value()) {
    return 0.0;
  }
  const uint32_t trip_count = tune_trip_count(*seed, 64, options.target_ns);
  spec.trip_count = trip_count;
  kernel = build_kernel(spec);
  if (!kernel.has_value()) {
    return 0.0;
  }

  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
  for (int i = 0; i < 2; ++i) {
    auto program =
        ThreadedMachinePreInstantiation::create(kernel->code,
                                                kernel->initial_memory, 0,
                                                kernel->memory_bytes,
                                                /*trace=*/false);
    if (!program.has_value()) {
      return 0.0;
    }
    programs.push_back(
        std::make_unique<ThreadedMachinePreInstantiation>(std::move(*program)));
  }
  std::vector<MeasurementPoint> points = {{.label = "noise/a",
                                           .x = 0.0,
                                           .program = programs[0].get(),
                                           .trip_count = trip_count},
                                          {.label = "noise/b",
                                           .x = 1.0,
                                           .program = programs[1].get(),
                                           .trip_count = trip_count}};
  measure_interleaved(points, {.repeats = options.repeats,
                               .warmup_runs = options.warmup_runs});
  const double a = points[0].median();
  const double b = points[1].median();
  if (a <= 0.0) {
    return 0.0;
  }
  // Reported per instruction, so it is directly comparable to a slope.
  return std::abs(a - b) / static_cast<double>(options.body_slots);
}

struct SweepPlan {
  KernelSpec spec;
  std::string branch_mode;
};

// The context axes each op is measured on. An opcode does not have one cost;
// it has a range, and which end of that range applies is the guest author's
// choice, not ours. Gas has to price the end an adversary can force, so the
// harness measures both ends explicitly rather than averaging them away.
std::vector<SweepPlan>
plan_sweeps(const Options &options)
{
  std::vector<SweepPlan> plans;
  for (size_t index = 0; index < kOpCount; ++index) {
    const Op op = static_cast<Op>(index);
    if (!is_measurable_target(op)) {
      continue;
    }
    if (!options.only_op.empty() && op_name(op) != options.only_op) {
      continue;
    }
    if (options.only_op == "PAIR") {
      continue;
    }
    KernelSpec base;
    base.target = op;
    base.body_slots = options.body_slots;

    if (is_branch_target(op)) {
      // Not-taken and taken are different amounts of engine work: both end the
      // run, but the taken path also resolves a target and re-enters there.
      KernelSpec not_taken = base;
      not_taken.branch_taken = false;
      plans.push_back({not_taken, "not_taken"});
      KernelSpec taken = base;
      taken.branch_taken = true;
      plans.push_back({taken, "taken"});
      continue;
    }
    if (is_memory_target(op)) {
      // Guest memory is capped at 64 KiB, so these two working sets bracket
      // every cache state a guest can put the engine in.
      KernelSpec hot = base;
      hot.working_set = WorkingSet::kL1Region;
      plans.push_back({hot, ""});
      KernelSpec spread = base;
      spread.working_set = WorkingSet::kFullRange;
      plans.push_back({spread, ""});
      if (is_load_target(op)) {
        // Every load can chase, not just LW: the node just has to fit the
        // loaded width. This is the latency axis, and without it a load is
        // priced from throughput alone while every ALU op is priced from a
        // dependency chain -- an asymmetry that understates loads.
        KernelSpec chase = base;
        chase.working_set = WorkingSet::kPointerChase;
        plans.push_back({chase, ""});
      }
      else {
        // A store has no result to chain through; contending on one address is
        // the nearest measurable worst case.
        KernelSpec contended = base;
        contended.working_set = WorkingSet::kSameAddress;
        plans.push_back({contended, ""});
      }
      continue;
    }
    if (is_divide_target(op)) {
      // Independent divides measure divider throughput, which the engine's own
      // dispatch largely hides. The chain form divides by one so the dividend
      // keeps its magnitude and every divide waits on the previous one: that is
      // the latency-bound arrangement, and it is the one a guest picks when it
      // wants to buy the most work per gas.
      KernelSpec large = base;
      large.magnitude = OperandMagnitude::kLarge;
      plans.push_back({large, ""});
      KernelSpec small = base;
      small.magnitude = OperandMagnitude::kSmall;
      plans.push_back({small, ""});
      KernelSpec chained = base;
      chained.context = OperandContext::kChain;
      chained.magnitude = OperandMagnitude::kLarge;
      plans.push_back({chained, ""});
      continue;
    }
    KernelSpec independent = base;
    independent.context = OperandContext::kIndependent;
    plans.push_back({independent, ""});
    KernelSpec chain = base;
    chain.context = OperandContext::kChain;
    plans.push_back({chain, ""});
  }

  // Interaction sweeps. These are not opcodes and never price one; they exist
  // to test whether a pair can cost more than its two opcodes' individual worst
  // cases sum to. If one can, pricing each opcode at its own worst case does
  // not bound programs that use them together, which would be a hole in the
  // method rather than in any single number.
  if (options.only_op.empty() || options.only_op == "PAIR") {
    KernelSpec forward;
    forward.pair = PairKind::kStoreLoadForward;
    forward.working_set = WorkingSet::kL1Region;
    forward.body_slots = options.body_slots;
    plans.push_back({forward, ""});
    KernelSpec distinct = forward;
    distinct.pair = PairKind::kStoreLoadDistinct;
    plans.push_back({distinct, ""});
    KernelSpec call = forward;
    call.pair = PairKind::kAuipcJalr;
    plans.push_back({call, ""});
  }
  return plans;
}

void
emit_json(std::FILE *out, const Options &options, const Provenance &provenance,
          const BaselineResult &baseline, double noise_floor,
          const std::vector<SweepResult> &sweeps,
          const std::vector<WorkloadResult> &workloads)
{
  std::println(out, "{{");
  std::println(out, R"(  "schema": "rv32-gas-calibration/1",)");
  std::println(out, R"(  "engine": "ThreadedMachine",)");
  std::println(out, R"(  "traced": false,)");
  std::println(
      out,
      "  \"provenance\": {{\"engine_fingerprint\": \"{}\", "
      "\"engine_source_digest\": \"{}\", \"host\": \"{}\", "
      "\"kernel\": \"{}\", \"cpu_model\": \"{}\", \"cpu_count\": {}, "
      "\"compiler\": \"{}\", \"build_type\": \"{}\", \"cxx_flags\": \"{}\", "
      "\"optimized\": {}, \"assertions_enabled\": {}, \"production_build\": "
      "{}, \"source_revision\": \"{}\"}},",
      escape_json(provenance.engine_fingerprint),
      escape_json(provenance.engine_source_digest),
      escape_json(provenance.host), escape_json(provenance.kernel),
      escape_json(provenance.cpu_model), provenance.cpu_count,
      escape_json(provenance.compiler), escape_json(provenance.build_type),
      escape_json(provenance.cxx_flags),
      provenance.optimized ? "true" : "false",
      provenance.assertions_enabled ? "true" : "false",
      provenance.is_production_build() ? "true" : "false",
      escape_json(provenance.source_revision));
  std::println(
      out,
      "  \"config\": {{\"body_slots\": {}, \"repeats\": {}, \"warmup_runs\": "
      "{}, \"target_ns\": {:.0f}, \"placements\": {}, \"quick\": {}}},",
      options.body_slots, options.repeats, options.warmup_runs,
      options.target_ns, options.placements, options.quick ? "true" : "false");
  std::println(out,
               "  \"baseline\": {{\"ns_per_filler_instruction\": {:.6f}, "
               "\"r_squared\": {:.6f}, \"theil_sen_slope\": {:.6f}, "
               "\"worst_relative_mad\": {:.6f}, \"usable\": {}}},",
               baseline.ns_per_filler, baseline.fit.r_squared,
               baseline.fit.theil_sen_slope, baseline.worst_relative_mad,
               baseline.usable ? "true" : "false");
  std::println(out, "  \"noise_floor_ns_per_instruction\": {:.6f},",
               noise_floor);

  std::println(out, R"(  "opcodes": [)");
  for (size_t i = 0; i < sweeps.size(); ++i) {
    const SweepResult &sweep = sweeps[i];
    std::println(
        out,
        "    {{\"op\": \"{}\", \"context\": \"{}\", \"working_set\": \"{}\", "
        "\"magnitude\": \"{}\", \"branch_mode\": \"{}\", \"pair\": {}, "
        "\"pair_ops\": {}, \"ns_upper_bound\": {:.6f}, "
        "\"ns_saturated\": {:.6f}, \"saturated_note\": \"{}\", "
        "\"scheduled_gas\": "
        "{}, \"usable\": {}, \"rejected_because\": \"{}\", \"slope_ns\": "
        "{:.6f}, \"theil_sen_ns\": {:.6f}, \"r_squared\": {:.6f}, "
        "\"slope_stderr_ns\": {:.6f}, \"points\": {}, \"ns_per_instruction\": "
        "{:.6f}, \"worst_relative_mad\": {:.6f}, \"body_runs\": {}, "
        "\"max_run_gas_seen\": {}}}{}",
        escape_json(sweep.op), escape_json(sweep.context),
        escape_json(sweep.working_set), escape_json(sweep.magnitude),
        escape_json(sweep.branch_mode), sweep.is_pair ? "true" : "false",
        json_string_array(sweep.pair_ops), sweep.ns_upper_bound,
        sweep.ns_saturated, escape_json(sweep.saturated_note),
        sweep.scheduled_gas, sweep.usable ? "true" : "false",
        escape_json(sweep.rejected_because), sweep.fit.slope,
        sweep.fit.theil_sen_slope, sweep.fit.r_squared, sweep.fit.slope_stderr,
        sweep.measured_points, sweep.ns_per_instruction,
        sweep.worst_relative_mad, sweep.audit.body_runs,
        sweep.audit.max_run_gas_seen, i + 1 == sweeps.size() ? "" : ",");
  }
  std::println(out, "  ],");

  std::println(out, R"(  "workloads": [)");
  for (size_t i = 0; i < workloads.size(); ++i) {
    const WorkloadResult &workload = workloads[i];
    std::print(
        out,
        "    {{\"name\": \"{}\", \"ns_per_iteration\": {:.6f}, "
        "\"scheduled_gas_per_iteration\": {}, \"instructions_per_iteration\": "
        "{}, \"ns_per_gas\": {:.6f}, \"relative_mad\": {:.6f}, "
        "\"fused_slots\": {}, \"shape\": \"{}\", \"op_counts\": {{",
        escape_json(workload.name), workload.ns_per_iteration,
        workload.scheduled_gas_per_iteration,
        workload.instructions_per_iteration, workload.ns_per_gas,
        workload.relative_mad, workload.fused_slots,
        escape_json(workload.shape));
    bool first = true;
    for (const auto &[op, count] : workload.op_counts) {
      std::print(out, "{}\"{}\": {}", first ? "" : ", ", escape_json(op),
                 count);
      first = false;
    }
    std::println(out, "}}}}{}", i + 1 == workloads.size() ? "" : ",");
  }
  std::println(out, "  ]\n}}");
}

int
main_impl(int argc, char **argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quick") {
      options.quick = true;
      options.repeats = 5;
      options.target_ns = 700'000.0;
      options.placements = 2;
      options.body_slots = 256;
    }
    else if (arg.starts_with("--repeats=")) {
      options.repeats = static_cast<uint32_t>(std::atoi(argv[i] + 10));
    }
    else if (arg.starts_with("--body-slots=")) {
      options.body_slots = static_cast<uint32_t>(std::atoi(argv[i] + 13));
    }
    else if (arg.starts_with("--target-ns=")) {
      options.target_ns = std::atof(argv[i] + 12);
    }
    else if (arg.starts_with("--out=")) {
      options.out_path = argv[i] + 6;
    }
    else if (arg == "--allow-nonproduction") {
      options.allow_nonproduction = true;
    }
    else if (arg == "--fingerprint") {
      // No measurement, so this answers "did the engine change?" instantly and
      // deterministically, on any machine, without a tolerance.
      std::println("{}", engine_fingerprint());
      return 0;
    }
    else if (arg.starts_with("--placements=")) {
      options.placements = std::max(1, std::atoi(argv[i] + 13));
    }
    else if (arg.starts_with("--op=")) {
      options.only_op = argv[i] + 5;
    }
    else if (arg == "--help" || arg == "-h") {
      std::println(
          stderr,
          "usage: rv32_gas_calibrate [--quick] [--repeats=N] [--body-slots=N]\n"
          "                          [--target-ns=F] [--op=NAME] "
          "[--placements=N]\n"
          "                          [--out=PATH] [--allow-nonproduction]\n"
          "       rv32_gas_calibrate --fingerprint");
      return 0;
    }
    else {
      std::println(stderr, "unknown argument: {}", arg);
      return 2;
    }
  }

  const Provenance provenance = collect_provenance();
  std::println(stderr, "engine fingerprint: {}", provenance.engine_fingerprint);
  std::println(stderr, "host: {} ({}, {} cpus)", provenance.host,
               provenance.cpu_model, provenance.cpu_count);
  std::println(stderr, "build: {} {}",
               provenance.build_type.empty() ? "(non-CMake)"
                                             : provenance.build_type.c_str(),
               provenance.cxx_flags);
  // Refuse rather than warn. The measured engine is the compiled interpreter,
  // and a Debug or ad-hoc build is a different one: its numbers look entirely
  // reasonable and are silently about the wrong thing, which is the failure
  // mode this harness exists to avoid. A warning at the top of a few minutes
  // of output gets scrolled past; an exit does not.
  if (!provenance.is_production_build() && !options.allow_nonproduction) {
    std::println(
        stderr,
        "\nrefusing to measure a non-Release build.\n  The gas costs describe "
        "the interpreter the node runs, and that is the one the\n  project's "
        "Release configuration produces (-O3, -march, libc++ under clang, "
        "LTO).\n  Build with: scripts/build.sh -t Release rv32_gas_calibrate\n "
        " or use scripts/rv32_gas_calibrate.sh, which does that for you.\n  "
        "Pass --allow-nonproduction to override for harness development only.");
    return 2;
  }

  std::println(stderr, "measuring noise floor...");
  const double noise_floor = measure_noise_floor(options);
  std::println(stderr, "  noise floor: {:.4f} ns/instruction", noise_floor);

  std::println(stderr, "measuring filler baseline...");
  const BaselineResult baseline = run_baseline(options);
  std::println(stderr, "  ADD baseline: {:.4f} ns/instruction (R2={:.4f})",
               baseline.ns_per_filler, baseline.fit.r_squared);
  if (!baseline.usable) {
    std::println(stderr, "baseline sweep failed; per-opcode costs would have "
                         "no absolute anchor");
    return 1;
  }

  const std::vector<SweepPlan> plans = plan_sweeps(options);
  std::vector<SweepResult> sweeps;
  sweeps.reserve(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    const SweepPlan &plan = plans[i];
    std::println(stderr, "[{}/{}] {} {}{}{}{}", i + 1, plans.size(),
                 plan.spec.pair != PairKind::kNone ? pair_name(plan.spec.pair)
                                                   : op_name(plan.spec.target),
                 plan.spec.pair != PairKind::kNone
                     ? "pair"
                     : context_name(plan.spec.context),
                 plan.spec.working_set == WorkingSet::kNone
                     ? ""
                     : (" " + working_set_name(plan.spec.working_set)).c_str(),
                 plan.spec.magnitude == OperandMagnitude::kDefault
                     ? ""
                     : (" " + magnitude_name(plan.spec.magnitude)).c_str(),
                 plan.branch_mode.empty() ? ""
                                          : (" " + plan.branch_mode).c_str());
    sweeps.push_back(run_sweep(plan.spec, options, baseline.ns_per_filler,
                               plan.branch_mode));
    const SweepResult &result = sweeps.back();
    if (result.usable) {
      std::println(
          stderr,
          "        {:.4f} ns/instr (slope {:.4f} +/- {:.4f}, R2={:.4f})",
          result.ns_per_instruction, result.fit.slope, result.fit.slope_stderr,
          result.fit.r_squared);
    }
    else {
      std::println(stderr, "        rejected: {}", result.rejected_because);
    }
  }

  std::println(stderr, "measuring validation workloads...");
  const std::vector<WorkloadResult> workloads =
      measure_workloads(options.repeats, options.warmup_runs,
                        options.target_ns);

  std::FILE *out = stdout;
  if (!options.out_path.empty()) {
    out = std::fopen(options.out_path.c_str(), "w");
    if (out == nullptr) {
      std::println(stderr, "cannot open {}", options.out_path);
      return 1;
    }
  }
  emit_json(out, options, provenance, baseline, noise_floor, sweeps, workloads);
  if (out != stdout) {
    std::fclose(out);
    std::println(stderr, "wrote {}", options.out_path);
  }
  return 0;
}

} // namespace
} // namespace hogd::rv32::gas_calibration

// The handlers below format, and so can themselves throw, which the check
// counts as an escape. They still turn every other failure into a readable
// line, which is what matters for a harness that runs unattended from a
// periodic job.
int
main(int argc, char **argv) // NOLINT(bugprone-exception-escape)
{
  try {
    return hogd::rv32::gas_calibration::main_impl(argc, argv);
  } catch (const std::exception &error) {
    std::println(stderr, "rv32_gas_calibrate failed: {}", error.what());
    return 1;
  } catch (...) {
    std::println(stderr, "rv32_gas_calibrate failed with an unknown exception");
    return 1;
  }
}
