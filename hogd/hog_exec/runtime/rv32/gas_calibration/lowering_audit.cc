#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

namespace hogd::rv32::gas_calibration {
namespace {

uint32_t
word_at(std::span<const uint8_t> code, size_t index)
{
  const size_t offset = index * kInstructionBytes;
  return uint32_t{code[offset]} | (uint32_t{code[offset + 1]} << 8) |
         (uint32_t{code[offset + 2]} << 16) |
         (uint32_t{code[offset + 3]} << 24);
}

// The handler the lowering installs for `word` when nothing around it can
// fuse. The probe program brackets the word with the same R-type filler the
// kernel body uses, so the word sits mid-run with a non-fusible neighbour on
// each side -- structurally identical to how the kernel body places it. A
// branch's slot offsets (+4 or +8) stay inside the four-instruction probe.
//
// The probe's memory size is deliberately tiny: handler selection depends on
// the decoded instruction, not on how much guest memory exists (the size is a
// run-time argument to the load/store handlers), and sizing every probe at
// kDefaultMaxMemoryBytes would zero 64 KiB per lookup.
std::optional<Handler>
isolated_handler(uint32_t word)
{
  static thread_local std::unordered_map<uint32_t, std::optional<Handler>>
      cache;
  const auto cached = cache.find(word);
  if (cached != cache.end()) {
    return cached->second;
  }

  const std::vector<uint8_t> code =
      test_program::code_from_words({filler_instruction_word(0), word,
                                     filler_instruction_word(1), 0x00000073U});
  std::optional<Handler> handler;
  auto probe = ThreadedMachinePreInstantiation::create(code, {}, 0,
                                                       /*total_memory_size=*/64,
                                                       /*trace=*/false);
  if (probe.has_value()) {
    const std::span<const Slot> slots =
        ThreadedMachinePreInstantiationTestPeer::slots(*probe);
    if (slots.size() >= 2) {
      handler = slots[1].handler;
    }
  }
  cache.emplace(word, handler);
  return handler;
}

// A barrier ends a straight-line run: the next instruction starts a fresh one
// through enter_run.
bool
is_run_barrier(Op op)
{
  switch (op) {
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
  case Op::kJal:
  case Op::kJalr:
  case Op::kEcall:
  case Op::kEbreak:
  case Op::kUnimp:
    return true;
  default:
    return false;
  }
}

} // namespace

LoweringAudit
audit_body(std::span<const uint8_t> code, std::span<const uint8_t> memory,
           uint32_t memory_bytes, size_t body_start_index, size_t body_slots,
           bool fuse)
{
  LoweringAudit audit;
  audit.body_slots = body_slots;

  auto lowered =
      ThreadedMachinePreInstantiation::create(code, memory, 0, memory_bytes,
                                              /*trace=*/false, kMaxRunGas,
                                              fuse);
  if (!lowered.has_value()) {
    audit.rewritten_slots = body_slots;
    return audit;
  }
  const std::span<const Slot> slots =
      ThreadedMachinePreInstantiationTestPeer::slots(*lowered);

  bool first_recorded = false;
  bool previous_was_barrier = false;
  const auto decoded_is_barrier = [](uint32_t word) {
    const std::vector<uint8_t> single = test_program::code_from_words({word});
    const auto program = ThreadedProgram::create(single);
    if (!program.has_value()) {
      return false;
    }
    const auto instruction = program->instruction_at(0);
    return instruction.has_value() && is_run_barrier(instruction->op);
  };
  for (size_t i = 0; i < body_slots; ++i) {
    const size_t index = body_start_index + i;
    if (index >= slots.size()) {
      break;
    }
    const Slot &slot = slots[index];
    audit.max_run_gas_seen =
        std::max<uint64_t>(audit.max_run_gas_seen, slot.gas_to_run);
    const uint32_t word = word_at(code, index);
    if (i == 0 || previous_was_barrier) {
      ++audit.body_runs;
    }
    previous_was_barrier = decoded_is_barrier(word);
    const std::optional<Handler> plain = isolated_handler(word);
    if (!plain.has_value() || slot.handler != *plain) {
      ++audit.rewritten_slots;
      if (!first_recorded) {
        audit.first_rewritten_slot = i;
        first_recorded = true;
      }
    }
  }
  return audit;
}

LoweringAudit
audit_kernel(const Kernel &kernel, const KernelSpec &spec, bool fuse)
{
  return audit_body(kernel.code, kernel.initial_memory, kernel.memory_bytes,
                    kernel.body_start_pc / kInstructionBytes, spec.body_slots,
                    fuse);
}

} // namespace hogd::rv32::gas_calibration
