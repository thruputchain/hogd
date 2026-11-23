#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hogd::rv32::gas_calibration {

// Mixed guest programs retain production fusion and report time per gas.
// Their spread compares pricing across this corpus; it is not a wall-time
// bound for arbitrary guests or for syscall/storage work.
struct WorkloadResult {
  std::string name;
  std::string shape;
  double ns_per_iteration = 0.0;
  uint64_t scheduled_gas_per_iteration = 0;
  uint64_t instructions_per_iteration = 0;
  // ns of real work per unit of gas charged. The pricing quality metric.
  double ns_per_gas = 0.0;
  double relative_mad = 0.0;
  size_t samples = 0;
  // Body slots the lowering fused or split; the fusion headroom, in slots.
  size_t fused_slots = 0;
  // Per-iteration counts let the reporter evaluate proposed prices without
  // rerunning the workload.
  std::map<std::string, uint64_t> op_counts;
};

[[nodiscard]] std::vector<WorkloadResult>
measure_workloads(uint32_t repeats, uint32_t warmup_runs, double target_ns);

} // namespace hogd::rv32::gas_calibration
