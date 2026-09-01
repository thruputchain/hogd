#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ratio>
#include <span>
#include <utility>
#include <vector>

namespace hogd::rv32::gas_calibration {

double
median_of(std::vector<double> values)
{
  if (values.empty()) {
    return 0.0;
  }
  std::ranges::sort(values);
  const size_t mid = values.size() / 2;
  if (values.size() % 2 == 1) {
    return values[mid];
  }
  return (values[mid - 1] + values[mid]) / 2.0;
}

double
MeasurementPoint::median() const
{
  return median_of(samples);
}

double
MeasurementPoint::relative_mad() const
{
  const double center = median();
  if (center == 0.0 || samples.empty()) {
    return 0.0;
  }
  std::vector<double> deviations;
  deviations.reserve(samples.size());
  for (const double sample : samples) {
    deviations.push_back(std::abs(sample - center));
  }
  return median_of(std::move(deviations)) / center;
}

RunOutcome
run_once(ThreadedMachine &machine, HaltHost &host)
{
  machine.reset(kGasLimit);
  const auto start = std::chrono::steady_clock::now();
  const StepResult result = machine.run(host);
  const auto end = std::chrono::steady_clock::now();
  return {.elapsed_ns =
              std::chrono::duration<double, std::nano>(end - start).count(),
          .result = result,
          .trap = machine.trap(),
          .gas_consumed = kGasLimit - machine.gas_remaining()};
}

uint32_t
tune_trip_count(const ThreadedMachinePreInstantiation &program,
                uint32_t seed_trip_count, double target_ns)
{
  ThreadedMachine machine(program, kGasLimit);
  HaltHost host;
  // Two runs: the first pays for cold slot array and memory image, the second
  // is the number worth extrapolating from.
  (void)run_once(machine, host);
  const RunOutcome outcome = run_once(machine, host);
  if (outcome.elapsed_ns <= 0.0) {
    return 64;
  }
  const double scale = target_ns / outcome.elapsed_ns;
  const double tuned = static_cast<double>(seed_trip_count) * scale;
  return static_cast<uint32_t>(std::clamp(std::ceil(tuned), 1.0, 2047.0));
}

void
measure_interleaved(std::span<MeasurementPoint> points,
                    const MeasureConfig &config)
{
  std::vector<ThreadedMachine> machines;
  machines.reserve(points.size());
  for (const MeasurementPoint &point : points) {
    machines.emplace_back(*point.program, kGasLimit);
  }
  HaltHost host;

  for (uint32_t warmup = 0; warmup < config.warmup_runs; ++warmup) {
    for (size_t i = 0; i < points.size(); ++i) {
      (void)run_once(machines[i], host);
    }
  }

  for (MeasurementPoint &point : points) {
    point.samples.clear();
    point.samples.reserve(config.repeats);
  }
  for (uint32_t repeat = 0; repeat < config.repeats; ++repeat) {
    for (size_t i = 0; i < points.size(); ++i) {
      const RunOutcome outcome = run_once(machines[i], host);
      // A kernel that trapped or ran out of gas did a different amount of work
      // than its siblings; keep the sample out and mark the point so the
      // reporter can drop the whole sweep rather than fit a mixed line.
      if (outcome.result != StepResult::kHalted) {
        points[i].ran_clean = false;
        continue;
      }
      points[i].samples.push_back(outcome.elapsed_ns /
                                  static_cast<double>(points[i].trip_count));
    }
  }
}

LinearFit
fit_line(std::span<const MeasurementPoint> points)
{
  LinearFit fit;
  fit.points = points.size();
  if (points.size() < 2) {
    return fit;
  }

  double sum_x = 0.0;
  double sum_y = 0.0;
  for (const MeasurementPoint &point : points) {
    sum_x += point.x;
    sum_y += point.median();
  }
  const auto n = static_cast<double>(points.size());
  const double mean_x = sum_x / n;
  const double mean_y = sum_y / n;

  double covariance = 0.0;
  double variance_x = 0.0;
  for (const MeasurementPoint &point : points) {
    const double dx = point.x - mean_x;
    covariance += dx * (point.median() - mean_y);
    variance_x += dx * dx;
  }
  if (variance_x == 0.0) {
    return fit;
  }
  fit.slope = covariance / variance_x;
  fit.intercept = mean_y - (fit.slope * mean_x);

  double residual_sum = 0.0;
  double total_sum = 0.0;
  for (const MeasurementPoint &point : points) {
    const double predicted = fit.intercept + (fit.slope * point.x);
    const double observed = point.median();
    residual_sum += (observed - predicted) * (observed - predicted);
    total_sum += (observed - mean_y) * (observed - mean_y);
  }
  fit.r_squared = total_sum == 0.0 ? 1.0 : 1.0 - (residual_sum / total_sum);
  if (points.size() > 2) {
    const double residual_variance =
        residual_sum / static_cast<double>(points.size() - 2);
    fit.slope_stderr = std::sqrt(residual_variance / variance_x);
  }

  std::vector<double> pairwise;
  pairwise.reserve(points.size() * (points.size() - 1) / 2);
  for (size_t i = 0; i < points.size(); ++i) {
    for (size_t j = i + 1; j < points.size(); ++j) {
      const double dx = points[j].x - points[i].x;
      if (dx != 0.0) {
        pairwise.push_back((points[j].median() - points[i].median()) / dx);
      }
    }
  }
  fit.theil_sen_slope = median_of(std::move(pairwise));
  return fit;
}

} // namespace hogd::rv32::gas_calibration
