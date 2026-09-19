#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace hogd::rv32::gas_calibration {

// The host the opcode kernels run under: an ecall halts. Those kernels never
// make a real host call, so no host work enters their measurement -- their
// single ecall is the terminal one that ends the run.
//
// The syscall-seam kernels run under SyscallDispatchHost instead, which
// returns rather than halting so a body can be made of calls. That is why the
// functions below take a Host& rather than a HaltHost&: the sweep machinery
// above them is the same for both, which is the whole reason the seam
// measurement did not need a second harness.
class HaltHost : public Host {
public:
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    return HostResult::halt(view.reg(10));
  }
};

// Budget every kernel runs under. Large enough that no sweep point can go
// out-of-gas, since an OOG kernel would stop early and time a different amount
// of work than its siblings.
inline constexpr uint64_t kGasLimit = 1'000'000'000'000ULL;

struct RunOutcome {
  double elapsed_ns = 0.0;
  StepResult result = StepResult::kTrap;
  Trap trap = Trap::kNone;
  uint64_t gas_consumed = 0;
};

// Resets and runs one kernel, timing only run(). The reset (which restores the
// 64 KiB memory image) stays outside the timed region.
[[nodiscard]] RunOutcome run_once(ThreadedMachine &machine, Host &host);

// Picks a trip count that makes one run of `program` take at least
// `target_ns`, so the measurement dominates clock resolution and per-run
// constant overhead. Returns the multiplier to apply to a seed trip count.
//
// Takes the host the sweep will actually be measured under: a syscall kernel
// tuned against a halting host would stop at its first body ecall and time a
// few instructions instead of a loop, then extrapolate a trip count from it.
[[nodiscard]] uint32_t
tune_trip_count(const ThreadedMachinePreInstantiation & /*program*/, Host &host,
                uint32_t seed_trip_count, double target_ns);

// One kernel in a sweep, with its independent-variable value and its samples.
struct MeasurementPoint {
  std::string label;
  // Independent variable. For an opcode sweep this is the number of target
  // slots in the body; for the baseline sweep it is the body slot count.
  double x = 0.0;
  const ThreadedMachinePreInstantiation *program = nullptr;
  uint32_t trip_count = 0;
  // ns per loop iteration, one entry per repeat.
  std::vector<double> samples;
  bool ran_clean = true;
  // Zero disables this check for diagnostic sweeps. Validation workloads
  // require the exact gas for the prologue, all iterations, and terminal.
  uint64_t expected_gas = 0;

  [[nodiscard]] double median() const;
  // Median absolute deviation over the repeats, relative to the median. The
  // per-point noise measure: a high value means this point's samples are not
  // reproducible and its contribution to the fit should not be trusted.
  [[nodiscard]] double relative_mad() const;
};

struct MeasureConfig {
  uint32_t repeats = 9;
  uint32_t warmup_runs = 2;
};

// Runs every point `repeats` times in round-robin order rather than finishing
// one point before starting the next. Interleaving matters: the dominant error
// on a shared or thermally-throttled machine is slow drift, and measuring
// points sequentially aliases that drift straight into the slope. Round-robin
// spreads any drift across all points, where the per-point median removes it.
void measure_interleaved(std::span<MeasurementPoint> points, Host &host,
                         const MeasureConfig &config);

struct LinearFit {
  // ns per unit of x. For an opcode sweep: ns per extra instance of the target
  // instruction, relative to the filler it displaced.
  double slope = 0.0;
  double intercept = 0.0;
  // Fraction of variance explained. A low value means the response is not
  // linear in x -- the kernel is measuring something other than "one more
  // instruction of this kind", and the slope should not be used.
  double r_squared = 0.0;
  // Median of pairwise slopes. Insensitive to a single outlying point, so a
  // large gap between this and `slope` flags an outlier the OLS fit absorbed.
  double theil_sen_slope = 0.0;
  // Standard error of the slope, from the fit's own residuals. This is the
  // uncertainty that matters: unlike a separately measured timer noise floor,
  // it includes the per-kernel systematic offsets (code layout, predictor
  // state) that repeating a measurement cannot average away. A slope smaller
  // than about twice this is not distinguishable from zero, and an opcode
  // whose slope is not distinguishable from zero costs what the filler costs.
  double slope_stderr = 0.0;
  size_t points = 0;
};

[[nodiscard]] LinearFit fit_line(std::span<const MeasurementPoint> points);

[[nodiscard]] double median_of(std::vector<double> values);

} // namespace hogd::rv32::gas_calibration
