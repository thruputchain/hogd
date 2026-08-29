#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/arch_trace.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace {

using hogd::rv32::describe_trace_divergence;
using hogd::rv32::ExecTrace;
using hogd::rv32::kInstructionBytes;
using hogd::rv32::kRegisterCount;
using hogd::rv32::load_u32_le;
using hogd::rv32::Machine;
using hogd::rv32::Program;
using hogd::rv32::strip_trace_gas;
using hogd::rv32::ThreadedMachine;
using hogd::rv32::ThreadedMachinePreInstantiation;
using hogd::rv32::trace_execution;
using hogd::rv32::TraceObserver;
using hogd::rv32::Trap;
namespace fuzz = hogd::rv32::fuzz;

[[noreturn]] void
fail(const char *what, const fuzz::EngineDiffInput &in)
{
  std::println(stderr, "rv32 engine divergence: {}", what);
  std::print(stderr,
             "  gas_budget={} memory_bytes={} max_run_gas={} method_id={:#x}"
             " host_seed={:#x} memory_start={} image_len={} words:",
             in.gas_budget, in.memory_bytes, in.max_run_gas, in.method_id,
             in.host_seed, in.memory_start, in.initial_memory.size());
  const std::span<const uint8_t> code(in.code);
  for (std::size_t i = 0; i + kInstructionBytes <= code.size();
       i += kInstructionBytes) {
    std::print(stderr, " {:08x}", load_u32_le(code, i));
  }
  std::println(stderr, "");
  std::abort();
}

// Compare the untraced (production handler set) threaded machine's final
// observable state against the canonical machine's. Only called when neither
// engine OOGed: on OOG the deficit design makes just the
// (trap, gas_remaining==0) pair consensus-visible, and the harness's OOG gate
// has already checked it.
void
check_untraced_final_state(const Machine &cmachine, ThreadedMachine &umachine,
                           const fuzz::EngineDiffInput &in)
{
  if (cmachine.trap() != umachine.trap()) {
    fail("untraced trap mismatch", in);
  }
  if (cmachine.halted() != umachine.halted() ||
      cmachine.halt_code() != umachine.halt_code()) {
    fail("untraced halt state mismatch", in);
  }
  if (cmachine.pc() != umachine.pc()) {
    fail("untraced pc mismatch", in);
  }
  if (cmachine.gas_remaining() != umachine.gas_remaining()) {
    fail("untraced gas_remaining mismatch", in);
  }
  for (std::size_t i = 0; i < kRegisterCount; ++i) {
    if (cmachine.reg(i) != umachine.reg(i)) {
      fail("untraced register mismatch", in);
    }
  }
  const std::span<const uint8_t> cmem = cmachine.memory();
  const std::span<const uint8_t> umem = umachine.memory();
  if (!std::ranges::equal(cmem, umem)) {
    fail("untraced memory mismatch", in);
  }
}

bool
same_call_log(const std::vector<hogd::rv32::EcallArgs> &a,
              const std::vector<hogd::rv32::EcallArgs> &b)
{
  return a.size() == b.size() &&
         std::ranges::equal(a, b, fuzz::ecall_args_equal);
}

} // namespace

extern "C" int
// NOLINTNEXTLINE(readability-identifier-naming)
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  const auto in =
      fuzz::carve_engine_diff_input(std::span<const uint8_t>(data, size));
  if (!in.has_value()) {
    return 0;
  }
  const std::span<const uint8_t> code(in->code);

  const auto canonical_err = fuzz::canonical_scan_error(code);
  const auto threaded_err = fuzz::threaded_scan_error(code);
  if (canonical_err != threaded_err) {
    fail("decoder ProgramError mismatch", *in);
  }

  // The production decode gate must agree on accept/reject. Its backward
  // lowering pass may report a different error *word* than the canonical
  // forward scan on multi-error programs (see production_scan_error), so only
  // the verdict is compared, never the error code.
  const auto production_err = fuzz::production_scan_error(code);
  if (production_err.has_value() != canonical_err.has_value()) {
    fail("production create accept/reject mismatch", *in);
  }
  if (canonical_err.has_value()) {
    return 0;
  }

  auto canonical_program = Program::create(code);
  if (!canonical_program.has_value()) {
    return 0;
  }
  const auto program =
      std::make_shared<const Program>(std::move(*canonical_program));

  // The carve guarantees memory_start + image size <= memory_bytes <=
  // kFuzzMemCap << kMaxMemoryBytes, so with the code already decode-clean
  // these creates can only succeed; treat a reject as divergence.
  auto pre = ThreadedMachinePreInstantiation::create(
      code, in->initial_memory, in->memory_start,
      /*total_memory_size=*/in->memory_bytes, /*trace=*/true, in->max_run_gas);
  auto pre_untraced = ThreadedMachinePreInstantiation::create(
      code, in->initial_memory, in->memory_start,
      /*total_memory_size=*/in->memory_bytes, /*trace=*/false, in->max_run_gas);
  if (!pre.has_value() || !pre_untraced.has_value()) {
    fail("production create rejected decode-clean code", *in);
  }

  Machine cmachine(program, in->memory_bytes, in->gas_budget);
  // Stage the initial image the way the runtime does: directly into guest
  // data memory (ThreadedMachinePreInstantiation bakes the same image).
  std::ranges::copy(in->initial_memory,
                    cmachine.memory().begin() + in->memory_start);
  cmachine.seed_method_id(in->method_id);
  fuzz::ScriptedHost chost(in->host_seed);
  const ExecTrace ctrace =
      trace_execution(cmachine, chost, fuzz::kFuzzTraceMaxSteps);

  ThreadedMachine tmachine(*pre, in->gas_budget);
  tmachine.seed_method_id(in->method_id);
  fuzz::ScriptedHost thost(in->host_seed);
  TraceObserver tobs;
  tmachine.run(thost, tobs);

  ThreadedMachine umachine(*pre_untraced, in->gas_budget);
  umachine.seed_method_id(in->method_id);
  fuzz::ScriptedHost uhost(in->host_seed);
  umachine.run(uhost);

  // The scripted schedule is a pure function of (seed, call index), so every
  // engine must have driven its host through the same call sequence.
  if (!same_call_log(chost.calls(), thost.calls()) ||
      !same_call_log(chost.calls(), uhost.calls())) {
    fail("host call log mismatch", *in);
  }

  const bool canonical_oog = cmachine.trap() == Trap::kOutOfGas;
  const bool threaded_oog = tmachine.trap() == Trap::kOutOfGas;
  const bool untraced_oog = umachine.trap() == Trap::kOutOfGas;
  if (canonical_oog != threaded_oog || canonical_oog != untraced_oog) {
    fail("OOG classification mismatch", *in);
  }
  if (canonical_oog) {
    if (cmachine.gas_remaining() != 0 || tmachine.gas_remaining() != 0 ||
        umachine.gas_remaining() != 0) {
      fail("OOG did not saturate gas_remaining to zero", *in);
    }
    return 0;
  }

  const ExecTrace cstripped = strip_trace_gas(ctrace);
  const ExecTrace tstripped = strip_trace_gas(tobs.trace());
  const std::string divergence =
      describe_trace_divergence(cstripped, tstripped);
  if (!divergence.empty()) {
    fail(divergence.c_str(), *in);
  }
  if (cstripped.steps.size() != tstripped.steps.size()) {
    fail("step count mismatch", *in);
  }
  for (std::size_t i = 0; i < cstripped.steps.size(); ++i) {
    if (cstripped.steps[i].state_before != tstripped.steps[i].state_before) {
      fail("state_before mismatch", *in);
    }
  }
  if (cmachine.gas_remaining() != tmachine.gas_remaining()) {
    fail("final gas_remaining mismatch", *in);
  }

  check_untraced_final_state(cmachine, umachine, *in);
  return 0;
}
