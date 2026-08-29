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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.h"
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
  uint64_t scheduled_gas = 0;
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

// Kernels for one sweep, kept alive together: the pre-instantiations borrow
// nothing from the Kernel, but the Kernel carries the audit inputs and the
// scheduled gas the reporter needs.
struct SweepKernels {
  std::vector<Kernel> kernels;
  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
  std::vector<uint32_t> target_counts;
};

std::optional<SweepKernels>
build_sweep(const KernelSpec &base, std::span<const uint32_t> target_counts,
            uint32_t trip_count, uint32_t placements)
{
  SweepKernels sweep;
  for (const uint32_t count : target_counts) {
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
      spec.placement_phase = phase * 2;
      std::optional<Kernel> kernel = build_kernel(spec);
      if (!kernel.has_value()) {
        return std::nullopt;
      }
      auto program =
          ThreadedMachinePreInstantiation::create(kernel->code,
                                                  kernel->initial_memory, 0,
                                                  kernel->memory_bytes,
                                                  /*trace=*/false);
      if (!program.has_value()) {
        return std::nullopt;
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
sweep_counts(uint32_t body_slots, bool quick)
{
  const uint32_t cap = (body_slots - 2) / 2;
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
  result.op = op_name(base.target);
  result.context = context_name(base.context);
  result.working_set = working_set_name(base.working_set);
  result.magnitude = magnitude_name(base.magnitude);
  result.branch_mode = branch_mode;
  result.scheduled_gas = scheduled_gas_for(base.target);

  const std::vector<uint32_t> counts =
      sweep_counts(options.body_slots, options.quick);
  std::optional<SweepKernels> sweep =
      build_sweep(base, counts, 64, options.placements);
  if (!sweep.has_value()) {
    result.rejected_because = "kernel not representable";
    return result;
  }

  // Audit every kernel in the sweep, not just the densest. A fit is a
  // statement about the difference between these specific programs, so a
  // fusion or run split in any one of them corrupts the slope -- and the
  // handler lookups are cached, so checking all of them costs little.
  KernelSpec audited = base;
  audited.trip_count = 64;
  for (size_t i = 0; i < sweep->kernels.size(); ++i) {
    audited.target_slots = sweep->target_counts[i];
    const LoweringAudit audit = audit_kernel(sweep->kernels[i], audited);
    if (i == sweep->kernels.size() - 1) {
      result.audit = audit;
    }
    if (!audit.clean()) {
      result.audit = audit;
      result.rejected_because =
          "lowering rewrote " + std::to_string(audit.rewritten_slots) +
          " body slot(s) of the " + std::to_string(sweep->target_counts[i]) +
          "-target kernel (first at " +
          std::to_string(audit.first_rewritten_slot) + ")";
      return result;
    }
  }

  const uint32_t trip_count =
      tune_trip_count(*sweep->programs.back(), 64, options.target_ns);
  sweep = build_sweep(base, counts, trip_count, options.placements);
  if (!sweep.has_value()) {
    result.rejected_because = "kernel not representable at tuned trip count";
    return result;
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
    std::optional<Kernel> kernel = build_kernel(spec);
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
    std::optional<Kernel> kernel = build_kernel(spec);
    auto program =
        ThreadedMachinePreInstantiation::create(kernel->code,
                                                kernel->initial_memory, 0,
                                                kernel->memory_bytes,
                                                /*trace=*/false);
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
  std::optional<Kernel> kernel = build_kernel(spec);
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
      if (op == Op::kLw) {
        KernelSpec chase = base;
        chase.working_set = WorkingSet::kPointerChase;
        plans.push_back({chase, ""});
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
  return plans;
}

void
emit_json(std::FILE *out, const Options &options, const Provenance &provenance,
          const BaselineResult &baseline, double noise_floor,
          const std::vector<SweepResult> &sweeps,
          const std::vector<WorkloadResult> &workloads)
{
  std::fprintf(out, "{\n");
  std::fprintf(out, "  \"schema\": \"rv32-gas-calibration/1\",\n");
  std::fprintf(out, "  \"engine\": \"ThreadedMachine\",\n");
  std::fprintf(out, "  \"traced\": false,\n");
  std::fprintf(out,
               "  \"provenance\": {\"engine_fingerprint\": \"%s\", "
               "\"host\": \"%s\", \"kernel\": \"%s\", "
               "\"cpu_model\": \"%s\", \"cpu_count\": %u, "
               "\"compiler\": \"%s\", \"build_type\": \"%s\", "
               "\"cxx_flags\": \"%s\", \"optimized\": %s, "
               "\"assertions_enabled\": %s, "
               "\"production_build\": %s, "
               "\"source_revision\": \"%s\"},\n",
               escape_json(provenance.engine_fingerprint).c_str(),
               escape_json(provenance.host).c_str(),
               escape_json(provenance.kernel).c_str(),
               escape_json(provenance.cpu_model).c_str(), provenance.cpu_count,
               escape_json(provenance.compiler).c_str(),
               escape_json(provenance.build_type).c_str(),
               escape_json(provenance.cxx_flags).c_str(),
               provenance.optimized ? "true" : "false",
               provenance.assertions_enabled ? "true" : "false",
               provenance.is_production_build() ? "true" : "false",
               escape_json(provenance.source_revision).c_str());
  std::fprintf(out,
               "  \"config\": {\"body_slots\": %u, \"repeats\": %u, "
               "\"warmup_runs\": %u, \"target_ns\": %.0f, \"placements\": %u, "
               "\"quick\": %s},\n",
               options.body_slots, options.repeats, options.warmup_runs,
               options.target_ns, options.placements,
               options.quick ? "true" : "false");
  std::fprintf(out,
               "  \"baseline\": {\"ns_per_filler_instruction\": %.6f, "
               "\"r_squared\": %.6f, \"theil_sen_slope\": %.6f, "
               "\"worst_relative_mad\": %.6f, \"usable\": %s},\n",
               baseline.ns_per_filler, baseline.fit.r_squared,
               baseline.fit.theil_sen_slope, baseline.worst_relative_mad,
               baseline.usable ? "true" : "false");
  std::fprintf(out, "  \"noise_floor_ns_per_instruction\": %.6f,\n",
               noise_floor);

  std::fprintf(out, "  \"opcodes\": [\n");
  for (size_t i = 0; i < sweeps.size(); ++i) {
    const SweepResult &sweep = sweeps[i];
    std::fprintf(
        out,
        "    {\"op\": \"%s\", \"context\": \"%s\", \"working_set\": \"%s\", "
        "\"magnitude\": \"%s\", \"branch_mode\": \"%s\", "
        "\"scheduled_gas\": %llu, \"usable\": %s, "
        "\"rejected_because\": \"%s\", \"slope_ns\": %.6f, "
        "\"theil_sen_ns\": %.6f, \"r_squared\": %.6f, "
        "\"slope_stderr_ns\": %.6f, \"points\": %zu, "
        "\"ns_per_instruction\": %.6f, \"worst_relative_mad\": %.6f, "
        "\"body_runs\": %zu, \"max_run_gas_seen\": %llu}%s\n",
        escape_json(sweep.op).c_str(), escape_json(sweep.context).c_str(),
        escape_json(sweep.working_set).c_str(),
        escape_json(sweep.magnitude).c_str(),
        escape_json(sweep.branch_mode).c_str(),
        static_cast<unsigned long long>(sweep.scheduled_gas),
        sweep.usable ? "true" : "false",
        escape_json(sweep.rejected_because).c_str(), sweep.fit.slope,
        sweep.fit.theil_sen_slope, sweep.fit.r_squared, sweep.fit.slope_stderr,
        sweep.measured_points, sweep.ns_per_instruction,
        sweep.worst_relative_mad, sweep.audit.body_runs,
        static_cast<unsigned long long>(sweep.audit.max_run_gas_seen),
        i + 1 == sweeps.size() ? "" : ",");
  }
  std::fprintf(out, "  ],\n");

  std::fprintf(out, "  \"workloads\": [\n");
  for (size_t i = 0; i < workloads.size(); ++i) {
    const WorkloadResult &workload = workloads[i];
    std::fprintf(
        out,
        "    {\"name\": \"%s\", \"ns_per_iteration\": %.6f, "
        "\"scheduled_gas_per_iteration\": %llu, "
        "\"instructions_per_iteration\": %llu, "
        "\"ns_per_gas\": %.6f, \"relative_mad\": %.6f, "
        "\"fused_slots\": %zu, \"shape\": \"%s\", "
        "\"op_counts\": {",
        escape_json(workload.name).c_str(), workload.ns_per_iteration,
        static_cast<unsigned long long>(workload.scheduled_gas_per_iteration),
        static_cast<unsigned long long>(workload.instructions_per_iteration),
        workload.ns_per_gas, workload.relative_mad, workload.fused_slots,
        escape_json(workload.shape).c_str());
    bool first = true;
    for (const auto &[op, count] : workload.op_counts) {
      std::fprintf(out, "%s\"%s\": %llu", first ? "" : ", ",
                   escape_json(op).c_str(),
                   static_cast<unsigned long long>(count));
      first = false;
    }
    std::fprintf(out, "}}%s\n", i + 1 == workloads.size() ? "" : ",");
  }
  std::fprintf(out, "  ]\n}\n");
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
      std::printf("%s\n", engine_fingerprint().c_str());
      return 0;
    }
    else if (arg.starts_with("--placements=")) {
      options.placements = std::max(1, std::atoi(argv[i] + 13));
    }
    else if (arg.starts_with("--op=")) {
      options.only_op = argv[i] + 5;
    }
    else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr, "usage: rv32_gas_calibrate [--quick] [--repeats=N] "
                           "[--body-slots=N] [--target-ns=F] [--op=NAME] "
                           "[--placements=N] [--out=PATH]\n");
      return 0;
    }
    else {
      std::fprintf(stderr, "unknown argument: %.*s\n",
                   static_cast<int>(arg.size()), arg.data());
      return 2;
    }
  }

  const Provenance provenance = collect_provenance();
  std::fprintf(stderr, "engine fingerprint: %s\n",
               provenance.engine_fingerprint.c_str());
  std::fprintf(stderr, "host: %s (%s, %u cpus)\n", provenance.host.c_str(),
               provenance.cpu_model.c_str(), provenance.cpu_count);
  std::fprintf(stderr, "build: %s %s\n",
               provenance.build_type.empty() ? "(non-CMake)"
                                             : provenance.build_type.c_str(),
               provenance.cxx_flags.c_str());
  // Refuse rather than warn. The measured engine is the compiled interpreter,
  // and a Debug or ad-hoc build is a different one: its numbers look entirely
  // reasonable and are silently about the wrong thing, which is the failure
  // mode this harness exists to avoid. A warning at the top of a few minutes
  // of output gets scrolled past; an exit does not.
  if (!provenance.is_production_build() && !options.allow_nonproduction) {
    std::fprintf(stderr,
                 "\nrefusing to measure a non-Release build.\n"
                 "  The gas costs describe the interpreter the node runs, and "
                 "that is the one the\n"
                 "  project's Release configuration produces (-O3, -march, "
                 "libc++ under clang, LTO).\n"
                 "  Build with: scripts/build.sh -t Release "
                 "rv32_gas_calibrate\n"
                 "  or use scripts/rv32_gas_calibrate.sh, which does that for "
                 "you.\n"
                 "  Pass --allow-nonproduction to override for harness "
                 "development only.\n");
    return 2;
  }

  std::fprintf(stderr, "measuring noise floor...\n");
  const double noise_floor = measure_noise_floor(options);
  std::fprintf(stderr, "  noise floor: %.4f ns/instruction\n", noise_floor);

  std::fprintf(stderr, "measuring filler baseline...\n");
  const BaselineResult baseline = run_baseline(options);
  std::fprintf(stderr, "  ADD baseline: %.4f ns/instruction (R2=%.4f)\n",
               baseline.ns_per_filler, baseline.fit.r_squared);
  if (!baseline.usable) {
    std::fprintf(stderr,
                 "baseline sweep failed; per-opcode costs would have no "
                 "absolute anchor\n");
    return 1;
  }

  const std::vector<SweepPlan> plans = plan_sweeps(options);
  std::vector<SweepResult> sweeps;
  sweeps.reserve(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    const SweepPlan &plan = plans[i];
    std::fprintf(stderr, "[%zu/%zu] %s %s%s%s%s\n", i + 1, plans.size(),
                 op_name(plan.spec.target).c_str(),
                 context_name(plan.spec.context).c_str(),
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
      std::fprintf(stderr,
                   "        %.4f ns/instr (slope %.4f +/- %.4f, R2=%.4f)\n",
                   result.ns_per_instruction, result.fit.slope,
                   result.fit.slope_stderr, result.fit.r_squared);
    }
    else {
      std::fprintf(stderr, "        rejected: %s\n",
                   result.rejected_because.c_str());
    }
  }

  std::fprintf(stderr, "measuring validation workloads...\n");
  const std::vector<WorkloadResult> workloads =
      measure_workloads(options.repeats, options.warmup_runs,
                        options.target_ns);

  std::FILE *out = stdout;
  if (!options.out_path.empty()) {
    out = std::fopen(options.out_path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", options.out_path.c_str());
      return 1;
    }
  }
  emit_json(out, options, provenance, baseline, noise_floor, sweeps, workloads);
  if (out != stdout) {
    std::fclose(out);
    std::fprintf(stderr, "wrote %s\n", options.out_path.c_str());
  }
  return 0;
}

} // namespace
} // namespace hogd::rv32::gas_calibration

int
main(int argc, char **argv)
{
  return hogd::rv32::gas_calibration::main_impl(argc, argv);
}
