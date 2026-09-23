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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/syscall_kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
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
  // The syscall-seam shapes. On by default: the crossing is a cost every
  // syscall pays and nothing else in the tree measures it, so a calibration
  // that skips it is missing a term rather than saving time (the whole seam
  // section is five sweeps, next to a hundred opcode ones).
  bool syscalls = true;
  bool syscalls_only = false;
};

// One measured (op, context) cell.
struct SweepResult {
  bool jump_kernel = false;
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
  // Opcode kernels make no host call: their single ecall is the terminal one.
  HaltHost halt_host;
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

  const uint32_t trip_count = tune_trip_count(*sweep->programs.back(),
                                              halt_host, 64, options.target_ns);
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
  measure_interleaved(points, halt_host,
                      {.repeats = options.repeats,
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
  // the filler's own measured absolute cost back gives an absolute number.
  //
  // A pair displaces two filler slots, so its slope is c_pair - 2*c_filler and
  // two have to come back. Adding one understated every pair row by a filler,
  // which is the difference between a pair clearing the "no usable cost"
  // rejection below and not.
  result.ns_per_instruction =
      (static_cast<double>(slots_per_target(base)) * filler_ns) +
      result.fit.slope;

  // An absolute upper bound, the fallback for a cell that cannot be saturated.
  //
  // The densest kernel runs `target_slots` targets and some fillers per
  // iteration, so
  //
  //     T = n_target * c_target + n_filler * c_filler + loop overhead
  //
  // and every term is non-negative, so T / n_target bounds c_target from above
  // -- no subtraction, no fit, no way to come out negative. It reads up to ~2x
  // high with the body half targets, and tightens as the target gets more
  // expensive, which is the right way round.
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
  // Everything above is a contrast, which goes negative as soon as layout
  // noise rivals the effect. This has no filler to subtract: its time over the
  // substitutions placed is directly what a program made of this instruction
  // costs, which is what worst-case pricing asks.
  {
    KernelSpec sat = base;
    sat.saturated = true;
    // A body of one repeated instruction is precisely the shape a fusion rule
    // matches, so for a single target lower it with fusion off and measure the
    // unfused cost directly. That is also the cost the schedule charges:
    // fusion is a saving the engine may or may not get, and worst-case pricing
    // cannot assume it.
    //
    // A pair row is the opposite -- auipc;jalr exists to measure the fused
    // form -- so those keep it on.
    const bool sat_fuse = base.pair != PairKind::kNone;
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
      auto seed = ThreadedMachinePreInstantiation::create(
          probe->code, probe->initial_memory, 0, probe->memory_bytes,
          /*trace=*/false, kMaxRunGas, sat_fuse);
      if (!seed.has_value()) {
        result.saturated_note = "saturated probe would not instantiate";
      }
      else {
        sat.trip_count =
            tune_trip_count(*seed, halt_host, 64, options.target_ns);
        auto kernel = build_kernel(sat);
        if (!kernel.has_value()) {
          result.saturated_note = "saturated kernel not representable at the "
                                  "tuned trip count: " +
                                  kernel_error_name(kernel.error());
        }
        else {
          const LoweringAudit audit = audit_kernel(*kernel, sat, sat_fuse);
          // Same intent test as the sweep audit above, and for the same reason:
          // a pair chosen *because* the engine fuses it has to fuse, and
          // audit.clean() asks the opposite question. Requiring clean() here
          // denied auipc;jalr the primary estimator permanently -- the one pair
          // whose whole purpose is the fused form -- and blamed it on "adjacent
          // copies of this opcode are a fusion rule", which is not what
          // happened.
          const bool expects_fusion = pair_expects_fusion(sat.pair);
          const bool matched = expects_fusion
                                   ? audit.rewritten_slots >= sat.target_slots
                                   : audit.clean();
          if (!matched) {
            result.saturated_note =
                expects_fusion
                    ? "saturated body fused only " +
                          std::to_string(audit.rewritten_slots) + " of " +
                          std::to_string(sat.target_slots) +
                          " pairs; this row exists to measure the fused form"
                    : "saturated body fused " +
                          std::to_string(audit.rewritten_slots) +
                          " slot(s) (first at " +
                          std::to_string(audit.first_rewritten_slot) +
                          "); adjacent copies of this opcode are a fusion "
                          "rule, "
                          "so a program cannot be made of it unfused";
          }
          else {
            auto program = ThreadedMachinePreInstantiation::create(
                kernel->code, kernel->initial_memory, 0, kernel->memory_bytes,
                false, kMaxRunGas, sat_fuse);
            if (!program.has_value()) {
              result.saturated_note = "saturated kernel would not instantiate";
            }
            else {
              std::vector<MeasurementPoint> one{
                  {.label = result.op + "/saturated",
                   .x = 0.0,
                   .program = &*program,
                   .trip_count = sat.trip_count}};
              measure_interleaved(one, halt_host,
                                  {.repeats = options.repeats,
                                   .warmup_runs = options.warmup_runs});
              if (!one[0].ran_clean) {
                result.saturated_note = "saturated kernel did not halt cleanly";
              }
              else {
                // Substitutions placed, not instructions retired. Units: a
                // pair's cost is quoted per pair, matching the ceiling it is
                // compared against. Direction: the iteration also runs two
                // boundary fillers and three loop instructions, and spreading
                // their time across those averages the target down, which is
                // the one direction worst-case pricing cannot tolerate.
                // Attributing all of it to the targets makes this an upper
                // bound, loose by about 1% at these body sizes.
                result.ns_saturated =
                    one[0].median() / static_cast<double>(kernel->target_units);
              }
            }
          }
        }
      }
    }
  }

  // A cell with no cost at all has nothing a schedule could use. Reject it
  // rather than pass a negative downstream, where worst-case pricing would
  // sort it to the bottom and treat the opcode as free. All three estimators
  // have to be consulted, in the order cost_ns() tries them.
  if (result.ns_saturated <= 0.0 && result.ns_upper_bound <= 0.0 &&
      result.ns_per_instruction <= 0.0) {
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
  HaltHost halt_host;
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
      tune_trip_count(*programs.back(), halt_host, 64, options.target_ns);
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

  measure_interleaved(points, halt_host,
                      {.repeats = options.repeats,
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
  HaltHost halt_host;
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
  const uint32_t trip_count =
      tune_trip_count(*seed, halt_host, 64, options.target_ns);
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
  measure_interleaved(points, halt_host,
                      {.repeats = options.repeats,
                       .warmup_runs = options.warmup_runs});
  const double a = points[0].median();
  const double b = points[1].median();
  if (a <= 0.0) {
    return 0.0;
  }
  // Reported per instruction, so it is directly comparable to a slope.
  return std::abs(a - b) / static_cast<double>(options.body_slots);
}

// One measured syscall-seam shape.
//
// The unit is a host call, not an instruction: what is being priced is the
// crossing, and a crossing is not divisible into instructions the way a body
// of ALU ops is.
struct SyscallSweepResult {
  std::string shape;
  std::string models;
  uint32_t guest_slots_per_call = 0;
  uint32_t output_bytes = 0;
  // Calls in the saturated body, the denominator its timing divides by.
  uint32_t calls_per_iteration = 0;
  // Instruction gas the current schedule charges for one call group. Host gas
  // is charged separately and is zero for every syscall these shapes model, so
  // this is the whole of what a guest is billed for the crossing today.
  uint64_t scheduled_gas_per_call = 0;
  // Primary: absolute ns per call of a body made entirely of call groups. A
  // level, not a contrast, for the reason the README gives at length.
  double ns_per_call = 0.0;
  // Diagnostic: the sweep's slope with the filler it displaced added back. It
  // answers a different question than ns_per_call -- what one more call costs
  // in a body that is mostly not calls -- and the two are expected to differ,
  // because the saturated body makes every ecall its own straight-line run
  // while the sparse one amortizes enter_run over the filler between them.
  double ns_per_call_marginal = 0.0;
  LinearFit fit;
  double worst_relative_mad = 0.0;
  LoweringAudit audit;
  size_t measured_points = 0;
  bool usable = false;
  std::string rejected_because;
};

// Kernels for one seam sweep, kept alive together: the pre-instantiations
// borrow nothing from the kernel, but the kernel carries the call count and the
// scheduled gas the checks below compare against.
struct SyscallKernels {
  std::vector<SyscallKernel> kernels;
  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
};

// Call-group counts for a seam sweep. No non-adjacency cap: ecall is a run
// barrier that matches no fusion rule in either position, so adjacent groups
// cannot create or destroy a fusion the way adjacent ALU ops can. The body can
// be filled.
std::vector<uint32_t>
syscall_sweep_counts(uint32_t body_slots, bool quick, SyscallShape shape)
{
  const uint32_t cap = max_call_groups(body_slots, shape);
  if (quick) {
    return {0, cap / 2, cap};
  }
  return {0, cap / 5, (2 * cap) / 5, (3 * cap) / 5, (4 * cap) / 5, cap};
}

SyscallSweepResult
run_syscall_sweep(SyscallShape shape, const Options &options, double filler_ns)
{
  SyscallSweepResult result;
  result.shape = syscall_shape_name(shape);
  result.models = syscall_shape_models(shape);
  result.guest_slots_per_call = syscall_shape_slots(shape);
  result.output_bytes = syscall_shape_output_bytes(shape);
  result.scheduled_gas_per_call =
      scheduled_gas_for(Op::kEcall) +
      ((result.guest_slots_per_call - 1) * scheduled_gas_for(Op::kAddi));

  const std::vector<uint32_t> counts =
      syscall_sweep_counts(options.body_slots, options.quick, shape);

  const auto build =
      [&](uint32_t trip_count) -> std::expected<SyscallKernels, KernelError> {
    SyscallKernels built;
    for (const uint32_t groups : counts) {
      SyscallKernelSpec spec;
      spec.shape = shape;
      spec.body_slots = options.body_slots;
      spec.call_groups = groups;
      spec.trip_count = trip_count;
      auto kernel = build_syscall_kernel(spec);
      if (!kernel.has_value()) {
        return std::unexpected(kernel.error());
      }
      auto program =
          ThreadedMachinePreInstantiation::create(kernel->code,
                                                  kernel->initial_memory, 0,
                                                  kernel->memory_bytes,
                                                  /*trace=*/false);
      if (!program.has_value()) {
        return std::unexpected(KernelError::kUndecodable);
      }
      built.kernels.push_back(std::move(*kernel));
      built.programs.push_back(
          std::make_unique<ThreadedMachinePreInstantiation>(
              std::move(*program)));
    }
    return built;
  };

  auto seeded = build(64);
  if (!seeded.has_value()) {
    result.rejected_because =
        "kernel not representable: " + kernel_error_name(seeded.error());
    return result;
  }
  SyscallDispatchHost tuning_host(shape);
  const uint32_t trip_count =
      tune_trip_count(*seeded->programs.back(), tuning_host, 64,
                      options.target_ns);
  auto built = build(trip_count);
  if (!built.has_value()) {
    result.rejected_because = "kernel not representable at the tuned trip "
                              "count: " +
                              kernel_error_name(built.error());
    return result;
  }
  const std::vector<SyscallKernel> &kernels = built->kernels;
  const auto &programs = built->programs;

  // The seam's audit, and it checks something the opcode audit cannot say for
  // it: that the body really is divided into one run per call. `body_runs`
  // counts barriers, so a body of N call groups must lower to N + 1 runs. If a
  // group failed to place, or an addi fused into its neighbour, or a slot did
  // not lower to an ecall at all, this is where it shows -- before any timing
  // is quoted against a call count that was never executed.
  for (size_t i = 0; i < kernels.size(); ++i) {
    const LoweringAudit audit =
        audit_body(kernels[i].code, kernels[i].initial_memory,
                   kernels[i].memory_bytes,
                   kernels[i].body_start_pc / kInstructionBytes,
                   options.body_slots);
    if (i == kernels.size() - 1) {
      result.audit = audit;
    }
    if (!audit.clean()) {
      result.audit = audit;
      result.rejected_because =
          "lowering rewrote " + std::to_string(audit.rewritten_slots) +
          " body slot(s) of the " + std::to_string(counts[i]) +
          "-call kernel (first at " +
          std::to_string(audit.first_rewritten_slot) + ")";
      return result;
    }
    const size_t expected_runs = counts[i] + 1;
    if (audit.body_runs != expected_runs) {
      result.audit = audit;
      result.rejected_because =
          "body lowered to " + std::to_string(audit.body_runs) +
          " runs, expected " + std::to_string(expected_runs) + " for " +
          std::to_string(counts[i]) + " calls";
      return result;
    }
  }

  // Untimed verification: run each kernel once under a fresh host and check it
  // made exactly the calls it was built to make, and that no bounds-checked
  // write through an out pointer faulted. A faulted write does less work than a
  // real one, and a miscounted body would be timed against the wrong
  // denominator -- neither leaves anything else to notice it by.
  for (size_t i = 0; i < kernels.size(); ++i) {
    SyscallDispatchHost verifier(shape);
    ThreadedMachine machine(*programs[i], kGasLimit);
    const RunOutcome outcome = run_once(machine, verifier);
    if (outcome.result != StepResult::kHalted) {
      result.rejected_because = "verification run did not halt cleanly";
      return result;
    }
    if (verifier.calls() != kernels[i].expected_host_calls) {
      result.rejected_because = "made " + std::to_string(verifier.calls()) +
                                " host calls, kernel "
                                "says " +
                                std::to_string(kernels[i].expected_host_calls);
      return result;
    }
    if (verifier.faulted()) {
      result.rejected_because = "a bounds-checked write through an out pointer "
                                "faulted; the host did less work than the "
                                "shape claims";
      return result;
    }
  }

  std::vector<MeasurementPoint> points;
  points.reserve(programs.size());
  for (size_t i = 0; i < programs.size(); ++i) {
    points.push_back({.label = result.shape + "/" + std::to_string(counts[i]),
                      .x = static_cast<double>(counts[i]),
                      .program = programs[i].get(),
                      .trip_count = trip_count});
  }
  SyscallDispatchHost host(shape);
  measure_interleaved(points, host,
                      {.repeats = options.repeats,
                       .warmup_runs = options.warmup_runs});
  for (const MeasurementPoint &point : points) {
    if (!point.ran_clean) {
      result.rejected_because = "kernel did not halt cleanly";
      return result;
    }
    result.worst_relative_mad =
        std::max(result.worst_relative_mad, point.relative_mad());
  }
  if (host.faulted()) {
    result.rejected_because = "an out-pointer write faulted during measurement";
    return result;
  }

  result.fit = fit_line(points);
  result.measured_points = points.size();
  result.calls_per_iteration = counts.back();
  // A call group displaces guest_slots_per_call filler slots, so that many
  // filler costs come back to turn the slope into an absolute number -- the
  // same accounting run_sweep does for a pair, for the same reason.
  result.ns_per_call_marginal =
      (static_cast<double>(result.guest_slots_per_call) * filler_ns) +
      result.fit.slope;
  if (result.calls_per_iteration > 0) {
    // The saturated body's iteration time over its call count. Two filler
    // slots (the fixed boundaries) and three loop-control instructions are in
    // that time and belong to no call; at 512 slots they are under half a
    // percent of it, which is an order of magnitude below the spread between
    // the shapes and two below the gate's tolerance. Subtracting them would
    // reintroduce the contrast this estimator exists to avoid.
    result.ns_per_call = points.back().median() /
                         static_cast<double>(result.calls_per_iteration);
  }
  result.usable = result.ns_per_call > 0.0;
  if (!result.usable) {
    result.rejected_because = "saturated body produced no usable timing";
  }
  return result;
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

    if (op == Op::kJal || op == Op::kJalr) {
      // These use dedicated control-flow kernels, not body-slot substitutions.
      continue;
    }
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

std::vector<JumpSpec>
plan_jumps(const Options &options)
{
  std::vector<JumpSpec> plans;
  for (const Op op : {Op::kJal, Op::kJalr}) {
    if (!options.only_op.empty() && options.only_op != op_name(op)) {
      continue;
    }
    for (const auto layout : {JumpLayout::kSequential, JumpLayout::kReverse,
                              JumpLayout::kShuffled}) {
      for (const uint32_t seed : {1U, 42U, 12345U}) {
        if (layout != JumpLayout::kShuffled && seed != 1) {
          continue;
        }
        const auto add = [&](OperandContext context, uint32_t nodes,
                             uint32_t stride) {
          plans.push_back({.target = op,
                           .context = context,
                           .layout = layout,
                           .nodes = nodes,
                           .stride_words = stride,
                           .seed = seed});
        };
        // Equal layouts and footprints for the direct/fixed-base/link-chain
        // comparison. The chain's 12-bit immediate limits its footprint.
        for (const auto context :
             {OperandContext::kIndependent, OperandContext::kNoLink,
              OperandContext::kChain}) {
          if (op == Op::kJal && context == OperandContext::kChain) {
            continue;
          }
          add(context, 64, 1);
          add(context, 480, 1);
          add(context, 64, 7);
        }
        if (op == Op::kJalr) {
          add(OperandContext::kTargetChase, 64, 1);
        }
        if (!options.quick) {
          if (op == Op::kJal) {
            add(OperandContext::kIndependent, 4096, 1);
            add(OperandContext::kNoLink, 512, 16);
          }
          else {
            // Same jalr PC, changing target at every visit. Includes dependent
            // loads and pad-return jumps; report the entire cost per dispatch
            // as an upper bound rather than subtracting a guessed load cost.
            add(OperandContext::kTargetChase, 4096, 1);
            add(OperandContext::kTargetChase, 512, 16);
          }
        }
      }
    }
  }
  return plans;
}

SweepResult
run_jump_sweep(JumpSpec spec, const Options &options)
{
  SweepResult result;
  result.jump_kernel = true;
  result.op = op_name(spec.target);
  result.context = jump_context_name(spec);
  result.working_set = "none";
  result.magnitude = "default";
  result.scheduled_gas = scheduled_gas_for(spec.target);
  result.saturated_note =
      spec.context == OperandContext::kTargetChase
          ? "sequence validation: 2 LW + JALR + JAL per dispatch, including "
            "loop/setup overhead; excluded from individual opcode pricing"
          : "absolute jump-chain bound, including loop setup/control; "
            "excluded from normalization";
  HaltHost host;
  auto kernel = build_jump_kernel(spec);
  if (!kernel) {
    throw std::runtime_error("jump kernel: " +
                             kernel_error_name(kernel.error()));
  }
  const auto lower = [](const Kernel &k) {
    return ThreadedMachinePreInstantiation::create(k.code, k.initial_memory, 0,
                                                   k.memory_bytes, false,
                                                   kMaxRunGas, false);
  };
  auto seed = lower(kernel->kernel);
  if (!seed) {
    throw std::runtime_error("jump probe did not lower");
  }
  spec.trip_count =
      tune_trip_count(*seed, host, spec.trip_count, options.target_ns);
  kernel = build_jump_kernel(spec);
  if (!kernel) {
    throw std::runtime_error("tuned jump kernel did not build");
  }
  auto program = lower(kernel->kernel);
  if (!program) {
    throw std::runtime_error("tuned jump kernel did not lower");
  }
  std::vector<MeasurementPoint> points{
      {.label = result.op + "/" + result.context,
       .x = 0.0,
       .program = &*program,
       .trip_count = spec.trip_count,
       .expected_gas = kernel->kernel.scheduled_gas}};
  measure_interleaved(points, host,
                      {.repeats = options.repeats,
                       .warmup_runs = options.warmup_runs});
  const auto &point = points.front();
  if (!point.ran_clean || point.samples.size() != options.repeats ||
      point.samples.size() < 3 || !std::isfinite(point.median()) ||
      point.median() <= 0) {
    throw std::runtime_error("invalid jump measurement: " + result.context);
  }
  result.ns_upper_bound = point.median() / kernel->kernel.target_units;
  result.ns_per_instruction = result.ns_upper_bound;
  result.worst_relative_mad = point.relative_mad();
  result.measured_points = point.samples.size();
  result.usable = true;
  return result;
}

void
emit_json(std::FILE *out, const Options &options, const Provenance &provenance,
          const BaselineResult &baseline, double noise_floor,
          const std::vector<SweepResult> &sweeps,
          const std::vector<SyscallSweepResult> &syscalls,
          const std::vector<WorkloadResult> &workloads)
{
  std::println(out, "{{");
  std::println(out, R"(  "schema": "rv32-gas-calibration/2",)");
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
        "    {{\"jump_kernel\": {}, \"op\": \"{}\", \"context\": \"{}\", "
        "\"working_set\": \"{}\", "
        "\"magnitude\": \"{}\", \"branch_mode\": \"{}\", \"pair\": {}, "
        "\"pair_ops\": {}, \"ns_upper_bound\": {:.6f}, "
        "\"ns_saturated\": {:.6f}, \"saturated_note\": \"{}\", "
        "\"scheduled_gas\": "
        "{}, \"usable\": {}, \"rejected_because\": \"{}\", \"slope_ns\": "
        "{:.6f}, \"theil_sen_ns\": {:.6f}, \"r_squared\": {:.6f}, "
        "\"slope_stderr_ns\": {:.6f}, \"points\": {}, \"ns_per_instruction\": "
        "{:.6f}, \"worst_relative_mad\": {:.6f}, \"body_runs\": {}, "
        "\"max_run_gas_seen\": {}}}{}",
        sweep.jump_kernel ? "true" : "false", escape_json(sweep.op),
        escape_json(sweep.context), escape_json(sweep.working_set),
        escape_json(sweep.magnitude), escape_json(sweep.branch_mode),
        sweep.is_pair ? "true" : "false", json_string_array(sweep.pair_ops),
        sweep.ns_upper_bound, sweep.ns_saturated,
        escape_json(sweep.saturated_note), sweep.scheduled_gas,
        sweep.usable ? "true" : "false", escape_json(sweep.rejected_because),
        sweep.fit.slope, sweep.fit.theil_sen_slope, sweep.fit.r_squared,
        sweep.fit.slope_stderr, sweep.measured_points, sweep.ns_per_instruction,
        sweep.worst_relative_mad, sweep.audit.body_runs,
        sweep.audit.max_run_gas_seen, i + 1 == sweeps.size() ? "" : ",");
  }
  std::println(out, "  ],");

  // Emitted whether or not any shape was measured, so a consumer can tell
  // "this run skipped the seam" from "this run predates the seam" by the
  // schema version rather than by an absent key.
  std::println(out, R"(  "syscalls": [)");
  for (size_t i = 0; i < syscalls.size(); ++i) {
    const SyscallSweepResult &row = syscalls[i];
    std::println(out,
                 "    {{\"shape\": \"{}\", \"models\": \"{}\", "
                 "\"guest_slots_per_call\": {}, \"output_bytes\": {}, "
                 "\"calls_per_iteration\": {}, \"scheduled_gas_per_call\": {}, "
                 "\"ns_per_call\": {:.6f}, \"ns_per_call_marginal\": {:.6f}, "
                 "\"slope_ns\": {:.6f}, \"theil_sen_ns\": {:.6f}, "
                 "\"r_squared\": {:.6f}, \"slope_stderr_ns\": {:.6f}, "
                 "\"points\": {}, \"worst_relative_mad\": {:.6f}, "
                 "\"body_runs\": {}, \"usable\": {}, "
                 "\"rejected_because\": \"{}\"}}{}",
                 escape_json(row.shape), escape_json(row.models),
                 row.guest_slots_per_call, row.output_bytes,
                 row.calls_per_iteration, row.scheduled_gas_per_call,
                 row.ns_per_call, row.ns_per_call_marginal, row.fit.slope,
                 row.fit.theil_sen_slope, row.fit.r_squared,
                 row.fit.slope_stderr, row.measured_points,
                 row.worst_relative_mad, row.audit.body_runs,
                 row.usable ? "true" : "false",
                 escape_json(row.rejected_because),
                 i + 1 == syscalls.size() ? "" : ",");
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
        "\"samples\": {}, \"ran_clean\": true, "
        "\"fused_slots\": {}, \"shape\": \"{}\", \"op_counts\": {{",
        escape_json(workload.name), workload.ns_per_iteration,
        workload.scheduled_gas_per_iteration,
        workload.instructions_per_iteration, workload.ns_per_gas,
        workload.relative_mad, workload.samples, workload.fused_slots,
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
    else if (arg == "--no-syscalls") {
      options.syscalls = false;
    }
    else if (arg == "--syscalls-only") {
      options.syscalls_only = true;
    }
    else if (arg == "--help" || arg == "-h") {
      std::println(
          stderr,
          "usage: rv32_gas_calibrate [--quick] [--repeats=N] [--body-slots=N]\n"
          "                          [--target-ns=F] [--op=NAME] "
          "[--placements=N]\n"
          "                          [--no-syscalls] [--syscalls-only]\n"
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

  const std::vector<SweepPlan> plans =
      options.syscalls_only ? std::vector<SweepPlan>{} : plan_sweeps(options);
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

  if (!options.syscalls_only) {
    for (const JumpSpec &spec : plan_jumps(options)) {
      std::println(stderr, "jump {} {}", op_name(spec.target),
                   jump_context_name(spec));
      sweeps.push_back(run_jump_sweep(spec, options));
      if (spec.context == OperandContext::kTargetChase) {
        std::println(stderr, "  {:.4f} ns/sequence (2 LW + JALR + JAL; "
                             "includes loop/setup overhead)",
                     sweeps.back().ns_upper_bound);
      }
      else {
        std::println(stderr, "  {:.4f} ns/jump (absolute upper bound)",
                     sweeps.back().ns_upper_bound);
      }
    }
  }

  std::vector<SyscallSweepResult> syscalls;
  if (options.syscalls) {
    std::println(stderr, "measuring syscall dispatch (the ecall seam; the "
                         "production syscall path is not linked here)...");
    syscalls.reserve(kAllSyscallShapes.size());
    for (size_t i = 0; i < kAllSyscallShapes.size(); ++i) {
      const SyscallShape shape = kAllSyscallShapes[i];
      std::println(stderr, "[{}/{}] syscall {}", i + 1,
                   kAllSyscallShapes.size(), syscall_shape_name(shape));
      syscalls.push_back(
          run_syscall_sweep(shape, options, baseline.ns_per_filler));
      const SyscallSweepResult &row = syscalls.back();
      if (row.usable) {
        // Against the filler baseline, which is what this driver has in hand.
        // rv32_gas_report.py normalizes by the median saturated opcode cell
        // instead -- the divisor the reference guards use -- so the two
        // multiples differ by design. Say which one this is.
        std::println(stderr,
                     "        {:.4f} ns/call ({:.2f}x the filler baseline; "
                     "marginal {:.4f})",
                     row.ns_per_call, row.ns_per_call / baseline.ns_per_filler,
                     row.ns_per_call_marginal);
      }
      else {
        std::println(stderr, "        rejected: {}", row.rejected_because);
      }
    }
  }

  std::println(stderr, "measuring validation workloads...");
  const std::vector<WorkloadResult> workloads =
      options.syscalls_only
          ? std::vector<WorkloadResult>{}
          : measure_workloads(options.repeats, options.warmup_runs,
                              options.target_ns);

  std::FILE *out = stdout;
  if (!options.out_path.empty()) {
    out = std::fopen(options.out_path.c_str(), "w");
    if (out == nullptr) {
      std::println(stderr, "cannot open {}", options.out_path);
      return 1;
    }
  }
  emit_json(out, options, provenance, baseline, noise_floor, sweeps, syscalls,
            workloads);
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
