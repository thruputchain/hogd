#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"

namespace {

using hogd::rv32::describe_trace_divergence;
using hogd::rv32::ExecTrace;
using hogd::rv32::kInstructionBytes;
using hogd::rv32::load_u32_le;
using hogd::rv32::Machine;
using hogd::rv32::Program;
using hogd::rv32::RecordingHost;
using hogd::rv32::strip_trace_gas;
using hogd::rv32::ThreadedMachine;
using hogd::rv32::ThreadedMachinePreInstantiation;
using hogd::rv32::trace_execution;
using hogd::rv32::TraceObserver;
using hogd::rv32::Trap;
namespace fuzz = hogd::rv32::fuzz;

[[noreturn]] void
fail(const char *what, std::span<const uint8_t> code, uint64_t gas,
     uint32_t memory_bytes)
{
  std::println(stderr, "rv32 engine divergence: {}", what);
  std::print(stderr, "  gas_budget={} memory_bytes={} words:", gas,
             memory_bytes);
  for (std::size_t i = 0; i + kInstructionBytes <= code.size();
       i += kInstructionBytes) {
    std::print(stderr, " {:08x}", load_u32_le(code, i));
  }
  std::println(stderr, "");
  std::abort();
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
    fail("decoder ProgramError mismatch", code, in->gas_budget,
         in->memory_bytes);
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

  auto pre = ThreadedMachinePreInstantiation::create(
      code, /*memory=*/{}, /*memory_start=*/0,
      /*total_memory_size=*/in->memory_bytes, /*trace=*/true);
  if (!pre.has_value()) {
    return 0;
  }

  Machine cmachine(program, in->memory_bytes, in->gas_budget);
  RecordingHost chost(RecordingHost::OnEcall::kContinue);
  const ExecTrace ctrace =
      trace_execution(cmachine, chost, fuzz::kFuzzTraceMaxSteps);

  ThreadedMachine tmachine(*pre, in->gas_budget);
  RecordingHost thost(RecordingHost::OnEcall::kContinue);
  TraceObserver tobs;
  tmachine.run(thost, tobs);

  const bool canonical_oog = cmachine.trap() == Trap::kOutOfGas;
  const bool threaded_oog = tmachine.trap() == Trap::kOutOfGas;
  if (canonical_oog != threaded_oog) {
    fail("OOG classification mismatch", code, in->gas_budget, in->memory_bytes);
  }
  if (canonical_oog || threaded_oog) {
    if (cmachine.gas_remaining() != 0 || tmachine.gas_remaining() != 0) {
      fail("OOG did not saturate gas_remaining to zero", code, in->gas_budget,
           in->memory_bytes);
    }
    return 0;
  }

  const ExecTrace cstripped = strip_trace_gas(ctrace);
  const ExecTrace tstripped = strip_trace_gas(tobs.trace());
  const std::string divergence =
      describe_trace_divergence(cstripped, tstripped);
  if (!divergence.empty()) {
    fail(divergence.c_str(), code, in->gas_budget, in->memory_bytes);
  }
  if (cstripped.steps.size() != tstripped.steps.size()) {
    fail("step count mismatch", code, in->gas_budget, in->memory_bytes);
  }
  for (std::size_t i = 0; i < cstripped.steps.size(); ++i) {
    if (cstripped.steps[i].state_before != tstripped.steps[i].state_before) {
      fail("state_before mismatch", code, in->gas_budget, in->memory_bytes);
    }
  }
  if (cmachine.gas_remaining() != tmachine.gas_remaining()) {
    fail("final gas_remaining mismatch", code, in->gas_budget,
         in->memory_bytes);
  }
  return 0;
}
