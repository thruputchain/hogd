#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hogd::rv32::gas_calibration {

// End-to-end check on a candidate gas schedule.
//
// The per-opcode sweeps deliberately suppress fusion so each slope is the cost
// of one instruction. Real guest code does not: the threaded lowering fuses
// nine patterns, so a fusible workload retires the same instructions -- and is
// charged the same gas, since gas is summed per decoded instruction before
// fusion -- in fewer dispatches. That makes gas conservative, but only if the
// gap is bounded, and the size of the gap is not something the microbenchmarks
// can show.
//
// These workloads measure it. What matters is not any single row but the
// spread of ns_per_gas across them: gas bounds wall time only as tightly as
// the worst ns_per_gas in the set, so max/min over this corpus is the pricing
// error a block gas limit has to absorb. A flat schedule shows a wide spread;
// a calibrated one should narrow it.
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
  // Opcode histogram for one iteration, keyed by schedule name. Lets the
  // reporter re-price a workload under a proposed schedule without re-running
  // anything -- which is what turns "here are some opcode costs" into "here is
  // what those costs would do to real code".
  std::map<std::string, uint64_t> op_counts;
};

[[nodiscard]] std::vector<WorkloadResult>
measure_workloads(uint32_t repeats, uint32_t warmup_runs, double target_ns);

} // namespace hogd::rv32::gas_calibration
