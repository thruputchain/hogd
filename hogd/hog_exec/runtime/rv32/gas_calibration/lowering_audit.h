#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace hogd::rv32 {

// `ThreadedMachinePreInstantiationTestPeer` is the friend seam
// threaded_machine.h declares for reading the baked Slot array.
// rv32_threaded_test.cc defines its own copy for fusion coverage; this one is
// the calibration harness's, and the two never link together.
struct ThreadedMachinePreInstantiationTestPeer {
  static std::span<const Slot>
  slots(const ThreadedMachinePreInstantiation &program)
  {
    return program.exec_;
  }
};

} // namespace hogd::rv32

namespace hogd::rv32::gas_calibration {

// What the production lowering actually built for a kernel.
//
// This exists because the measurement is only as good as the assumption that
// each body slot lowers to one dispatch of the instruction we think we put
// there. The threaded lowering fuses nine instruction patterns and splits runs
// that exceed kMaxRunGas, either of which silently changes the work per slot
// and would show up as a plausible-looking but wrong cost. The harness checks
// rather than assumes, and refuses to report a slope from a kernel whose
// lowering does not match its source.
struct LoweringAudit {
  size_t body_slots = 0;
  // Body slots whose baked handler differs from the handler the same encoded
  // word gets when lowered in isolation between two fillers. Nonzero means a
  // fusion rule or a mid-run break carrier fired inside the body.
  size_t rewritten_slots = 0;
  // First body slot index that was rewritten, for diagnostics.
  size_t first_rewritten_slot = 0;
  // Straight-line runs the body is divided into: one, plus one per barrier
  // (branch/jump/ecall) inside it. This is how a branch's extra enter_run shows
  // up structurally rather than only in the timing.
  size_t body_runs = 0;
  // Largest per-slot `gas_to_run` in the body. That field is the gas from a
  // slot through the end of its run -- not a run-head marker -- so its maximum
  // is the gas of the body's longest run. Approaching kMaxRunGas means the
  // lowering is about to start splitting runs with break carriers, which
  // changes handlers and invalidates a sweep.
  uint64_t max_run_gas_seen = 0;

  [[nodiscard]] bool clean() const { return rewritten_slots == 0; }
};

// Lowers `code` (untraced, exactly as production would) and compares each of
// the `body_slots` slots starting at `body_start_index` against an isolated
// lowering of the same encoded word.
[[nodiscard]] LoweringAudit
audit_body(std::span<const uint8_t> code, std::span<const uint8_t> memory,
           uint32_t memory_bytes, size_t body_start_index, size_t body_slots);

// audit_body over a calibration kernel's body. A calibration kernel is
// expected to come back clean; a validation workload deliberately is not.
[[nodiscard]] LoweringAudit audit_kernel(const Kernel &kernel,
                                         const KernelSpec &spec);

} // namespace hogd::rv32::gas_calibration
