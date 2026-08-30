#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

namespace hogd::rv32::gas_calibration {
namespace {

using test_program::append_u32;

// Operand values for kSrcLargeReg / kSrcSmallReg. Large-positive vs
// small-positive makes every conditional branch's not-taken direction
// expressible without a taken-branch's control-flow change (see
// branch_operands), and gives DIV a full-width dividend over a small divisor,
// which is the slow end of the x86 divider.
constexpr int32_t kSrcLargeValue = 0x5EEDBEEF;
constexpr int32_t kSrcSmallValue = 3;

constexpr uint32_t kL1RegionBase = 2048;    // covers [0, 4095] with +/-2048 imm
constexpr uint32_t kFullRangeStride = 8192; // 8 bases over the 64 KiB space
// One pointer-chase node per cache line, over the whole guest address space.
constexpr uint32_t kChaseStride = 64;

struct Encoding {
  uint32_t opcode = 0;
  uint8_t funct3 = 0;
  uint8_t funct7 = 0;
};

// RV32IM field encodings, keyed by Op. kernel_encoding_matches_decoder_test
// pins every entry against the production decoder rather than trusting this
// table.
Encoding
encoding_for(Op op)
{
  switch (op) {
  case Op::kLui:
    return {.opcode = 0x37};
  case Op::kAuipc:
    return {.opcode = 0x17};
  case Op::kBeq:
    return {.opcode = 0x63, .funct3 = 0};
  case Op::kBne:
    return {.opcode = 0x63, .funct3 = 1};
  case Op::kBlt:
    return {.opcode = 0x63, .funct3 = 4};
  case Op::kBge:
    return {.opcode = 0x63, .funct3 = 5};
  case Op::kBltu:
    return {.opcode = 0x63, .funct3 = 6};
  case Op::kBgeu:
    return {.opcode = 0x63, .funct3 = 7};
  case Op::kLb:
    return {.opcode = 0x03, .funct3 = 0};
  case Op::kLh:
    return {.opcode = 0x03, .funct3 = 1};
  case Op::kLw:
    return {.opcode = 0x03, .funct3 = 2};
  case Op::kLbu:
    return {.opcode = 0x03, .funct3 = 4};
  case Op::kLhu:
    return {.opcode = 0x03, .funct3 = 5};
  case Op::kSb:
    return {.opcode = 0x23, .funct3 = 0};
  case Op::kSh:
    return {.opcode = 0x23, .funct3 = 1};
  case Op::kSw:
    return {.opcode = 0x23, .funct3 = 2};
  case Op::kAddi:
    return {.opcode = 0x13, .funct3 = 0};
  case Op::kSlti:
    return {.opcode = 0x13, .funct3 = 2};
  case Op::kSltiu:
    return {.opcode = 0x13, .funct3 = 3};
  case Op::kXori:
    return {.opcode = 0x13, .funct3 = 4};
  case Op::kOri:
    return {.opcode = 0x13, .funct3 = 6};
  case Op::kAndi:
    return {.opcode = 0x13, .funct3 = 7};
  case Op::kSlli:
    return {.opcode = 0x13, .funct3 = 1};
  case Op::kSrli:
    return {.opcode = 0x13, .funct3 = 5};
  case Op::kSrai:
    return {.opcode = 0x13, .funct3 = 5, .funct7 = 0x20};
  case Op::kAdd:
    return {.opcode = 0x33, .funct3 = 0, .funct7 = 0x00};
  case Op::kSub:
    return {.opcode = 0x33, .funct3 = 0, .funct7 = 0x20};
  case Op::kSll:
    return {.opcode = 0x33, .funct3 = 1, .funct7 = 0x00};
  case Op::kSlt:
    return {.opcode = 0x33, .funct3 = 2, .funct7 = 0x00};
  case Op::kSltu:
    return {.opcode = 0x33, .funct3 = 3, .funct7 = 0x00};
  case Op::kXor:
    return {.opcode = 0x33, .funct3 = 4, .funct7 = 0x00};
  case Op::kSrl:
    return {.opcode = 0x33, .funct3 = 5, .funct7 = 0x00};
  case Op::kSra:
    return {.opcode = 0x33, .funct3 = 5, .funct7 = 0x20};
  case Op::kOr:
    return {.opcode = 0x33, .funct3 = 6, .funct7 = 0x00};
  case Op::kAnd:
    return {.opcode = 0x33, .funct3 = 7, .funct7 = 0x00};
  case Op::kMul:
    return {.opcode = 0x33, .funct3 = 0, .funct7 = 0x01};
  case Op::kMulh:
    return {.opcode = 0x33, .funct3 = 1, .funct7 = 0x01};
  case Op::kMulhsu:
    return {.opcode = 0x33, .funct3 = 2, .funct7 = 0x01};
  case Op::kMulhu:
    return {.opcode = 0x33, .funct3 = 3, .funct7 = 0x01};
  case Op::kDiv:
    return {.opcode = 0x33, .funct3 = 4, .funct7 = 0x01};
  case Op::kDivu:
    return {.opcode = 0x33, .funct3 = 5, .funct7 = 0x01};
  case Op::kRem:
    return {.opcode = 0x33, .funct3 = 6, .funct7 = 0x01};
  case Op::kRemu:
    return {.opcode = 0x33, .funct3 = 7, .funct7 = 0x01};
  default:
    return {};
  }
}

// Only referenced from an assert, so it disappears under NDEBUG.
[[maybe_unused]] bool
is_r_type(Op op)
{
  return encoding_for(op).opcode == 0x33;
}

bool
is_shift_immediate(Op op)
{
  return op == Op::kSlli || op == Op::kSrli || op == Op::kSrai;
}

bool
is_op_immediate(Op op)
{
  return encoding_for(op).opcode == 0x13;
}

bool
is_load(Op op)
{
  return encoding_for(op).opcode == 0x03;
}

bool
is_store(Op op)
{
  return encoding_for(op).opcode == 0x23;
}

// The filler: R-type ADD on a private register pair. No fusion rule in
// threaded_machine.h matches it as a head (the ALU-ALU rule wants sll/addi/srl,
// the rest want lui/auipc/load/store/addi) or as a tail (not or/addi/sll, not a
// load/store/branch), so a body of alternating filler and target lowers one
// slot per instruction. lowering_audit.h re-checks that per kernel.
// Load width in bytes, for keeping generated addresses naturally aligned.
uint32_t
memory_access_width(Op op)
{
  switch (op) {
  case Op::kLb:
  case Op::kLbu:
  case Op::kSb:
    return 1;
  case Op::kLh:
  case Op::kLhu:
  case Op::kSh:
    return 2;
  default:
    return 4;
  }
}

struct BranchOperands {
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t offset = 0;
};

// A body-slot branch must not change how many instructions the iteration
// retires, or the sweep's fixed-work invariant breaks.
//
// not-taken: the condition is false against (large, small) operands, so the
// branch falls through.
// taken: the condition is true and the target is the *next* instruction
// (offset +4), so control lands exactly where a fall-through would. That
// isolates the cost of the taken path (target resolution plus a fresh
// enter_run at the target) with the retired-instruction count held fixed.
BranchOperands
branch_operands(Op op, bool taken)
{
  const int32_t offset = taken ? 4 : 8;
  switch (op) {
  case Op::kBeq:
    return taken ? BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcLargeReg,
                                  .offset = offset}
                 : BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcSmallReg,
                                  .offset = offset};
  case Op::kBne:
    return taken ? BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcSmallReg,
                                  .offset = offset}
                 : BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcLargeReg,
                                  .offset = offset};
  case Op::kBlt:
  case Op::kBltu:
    return taken ? BranchOperands{.rs1 = kSrcSmallReg,
                                  .rs2 = kSrcLargeReg,
                                  .offset = offset}
                 : BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcSmallReg,
                                  .offset = offset};
  case Op::kBge:
  case Op::kBgeu:
    return taken ? BranchOperands{.rs1 = kSrcLargeReg,
                                  .rs2 = kSrcSmallReg,
                                  .offset = offset}
                 : BranchOperands{.rs1 = kSrcSmallReg,
                                  .rs2 = kSrcLargeReg,
                                  .offset = offset};
  default:
    return {};
  }
}

// Address base for memory slot `index`, and the offset that reaches the byte
// this slot touches.
//
// kL1Region: one base at 2048, offsets sweeping [-2048, 2047] -- exactly the
// low 4 KiB, 64 cache lines, comfortably L1d-resident.
// kFullRange: eight bases 8 KiB apart, each covering [base-2048, base+2047] --
// 32 KiB in eight disjoint chunks spread across the whole 64 KiB guest space,
// which is L1d-sized and so guarantees L1 pressure while staying inside L2.
// Guest memory is capped at kMaxMemoryBytes (64 KiB), so those two contexts
// bracket the entire cache behaviour a guest can reach; there is no
// DRAM-resident case to price.
struct MemoryOperand {
  uint8_t base_reg = 0;
  int32_t offset = 0;
};

MemoryOperand
memory_operand(WorkingSet working_set, uint32_t index, uint32_t width)
{
  const auto base_reg = static_cast<uint8_t>(
      kAddressBankBase +
      (working_set == WorkingSet::kFullRange ? index % kAddressBankSize : 0));
  // Stride by 64 bytes so successive accesses land on successive cache lines
  // rather than repeatedly hitting one; wrap inside the +/-2048 immediate.
  const int32_t span = 4096;
  const auto raw =
      static_cast<int32_t>((index * 64U) % static_cast<uint32_t>(span));
  int32_t offset = raw - 2048;
  offset -= offset % static_cast<int32_t>(width); // keep accesses aligned
  return {.base_reg = base_reg, .offset = offset};
}

// Writes a single shuffled cycle through every kChaseStride-aligned node of
// `memory`: the word at node i holds the address of node next(i). A shuffle
// rather than a fixed stride is the point -- a strided walk is prefetchable, so
// it would measure the prefetcher rather than the load latency the chase is
// there to expose. The permutation is a fixed-seed LCG so two runs of the
// harness walk the same cycle and their numbers are comparable.
void
seed_pointer_chase(std::vector<uint8_t> &memory)
{
  const uint32_t nodes = static_cast<uint32_t>(memory.size()) / kChaseStride;
  if (nodes < 2) {
    return;
  }
  std::vector<uint32_t> order(nodes);
  for (uint32_t i = 0; i < nodes; ++i) {
    order[i] = i;
  }
  // Fisher-Yates over order[1..] keeps node 0 first, so the chase starts where
  // the prologue points the chain register.
  uint64_t state = 0x9E3779B97F4A7C15ULL;
  for (uint32_t i = nodes - 1; i > 1; --i) {
    state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    const uint32_t j = 1 + static_cast<uint32_t>((state >> 33) % i);
    std::swap(order[i], order[j]);
  }
  for (uint32_t i = 0; i < nodes; ++i) {
    const uint32_t from = order[i] * kChaseStride;
    const uint32_t to = order[(i + 1) % nodes] * kChaseStride;
    memory[from] = static_cast<uint8_t>(to);
    memory[from + 1] = static_cast<uint8_t>(to >> 8);
    memory[from + 2] = static_cast<uint8_t>(to >> 16);
    memory[from + 3] = static_cast<uint8_t>(to >> 24);
  }
}

uint32_t
address_base_value(WorkingSet working_set, uint32_t index)
{
  if (working_set == WorkingSet::kFullRange) {
    return kL1RegionBase + (index * kFullRangeStride);
  }
  return kL1RegionBase;
}

// Encodes one target instance. `index` is the target's ordinal within the body,
// which drives the rotating destination bank and the memory address walk.
uint32_t
target_word(const KernelSpec &spec, uint32_t index)
{
  const Encoding encoding = encoding_for(spec.target);
  const bool chain = spec.context == OperandContext::kChain;

  if (is_load(spec.target) && spec.working_set == WorkingSet::kPointerChase) {
    // rd and rs1 are both the chain register: the address of this load is the
    // value the previous one produced.
    return test_program::load(kChainReg, encoding.funct3, kChainReg, 0);
  }
  if (is_load(spec.target)) {
    const uint32_t width = memory_access_width(spec.target);
    const MemoryOperand operand =
        memory_operand(spec.working_set, index, width);
    // Destination is drawn from a bank disjoint from the filler's operands, so
    // the following filler never consumes the loaded value and fuse_load_use
    // cannot pair them.
    const auto rd = static_cast<uint8_t>(kLoadDstBase + (index % kLoadDstSize));
    return test_program::load(rd, encoding.funct3, operand.base_reg,
                              operand.offset);
  }
  if (is_store(spec.target)) {
    const uint32_t width = memory_access_width(spec.target);
    const MemoryOperand operand =
        memory_operand(spec.working_set, index, width);
    return test_program::store(operand.base_reg, kStoreValueReg, operand.offset,
                               encoding.funct3);
  }
  if (spec.target == Op::kLui || spec.target == Op::kAuipc) {
    const uint8_t rd =
        chain ? kChainReg
              : static_cast<uint8_t>(kDataBankBase + (index % kDataBankSize));
    return test_program::u_type(encoding.opcode, rd, 0x0002F000U);
  }
  if (is_branch_target(spec.target)) {
    const BranchOperands operands =
        branch_operands(spec.target, spec.branch_taken);
    return test_program::branch(encoding.funct3, operands.rs1, operands.rs2,
                                operands.offset);
  }

  const uint8_t rd =
      chain ? kChainReg
            : static_cast<uint8_t>(kDataBankBase + (index % kDataBankSize));
  const uint8_t rs1 = chain ? kChainReg : kSrcLargeReg;

  if (chain && is_divide_target(spec.target)) {
    // Divide by one so the chained dividend keeps its magnitude for the whole
    // run. Every slot then waits on the previous divide's result, which is the
    // arrangement a guest author would use to force divider latency rather
    // than divider throughput -- and so the arrangement gas has to price.
    return test_program::op(kChainReg, kChainReg, kOneReg, encoding.funct3,
                            encoding.funct7);
  }

  if (is_shift_immediate(spec.target)) {
    const auto shamt = static_cast<int32_t>(1 + (index % 30));
    const int32_t imm = spec.target == Op::kSrai ? (shamt | 0x400) : shamt;
    return test_program::i_type(encoding.opcode, rd, encoding.funct3, rs1, imm);
  }
  if (is_op_immediate(spec.target)) {
    return test_program::i_type(encoding.opcode, rd, encoding.funct3, rs1,
                                kSrcSmallValue);
  }
  assert(is_r_type(spec.target));
  // Neither source register is ever written by the body, so an R-type target's
  // operand magnitudes are constant for the whole run. That is what makes the
  // DIV/REM magnitude classes mean anything: kLarge divides a full-width
  // dividend by a small divisor (the slow end of the hardware divider) and
  // kSmall divides small by small, and the value being divided never drifts
  // between them mid-run.
  const uint8_t effective_rs1 =
      spec.magnitude == OperandMagnitude::kSmall ? kSrcSmallReg : rs1;
  return test_program::op(rd, effective_rs1, kSrcSmallReg, encoding.funct3,
                          encoding.funct7);
}

} // namespace

std::expected<uint32_t, KernelError>
canonical_instruction_word(Op op, uint8_t reg)
{
  if (!is_measurable_target(op)) {
    return std::unexpected(KernelError::kUnsupportedTarget);
  }
  const Encoding encoding = encoding_for(op);
  if (op == Op::kLui || op == Op::kAuipc) {
    return test_program::u_type(encoding.opcode, reg, 0x0002F000U);
  }
  if (is_branch_target(op)) {
    // Forward by two instructions: in range for the short probe programs the
    // fingerprint lowers, which the branch-target fusion rule requires.
    return test_program::branch(encoding.funct3, reg, reg, 8);
  }
  if (is_load(op)) {
    return test_program::load(reg, encoding.funct3, reg, 0);
  }
  if (is_store(op)) {
    return test_program::store(reg, reg, 0, encoding.funct3);
  }
  if (is_shift_immediate(op)) {
    const int32_t imm = op == Op::kSrai ? (4 | 0x400) : 4;
    return test_program::i_type(encoding.opcode, reg, encoding.funct3, reg,
                                imm);
  }
  if (is_op_immediate(op)) {
    return test_program::i_type(encoding.opcode, reg, encoding.funct3, reg, 4);
  }
  return test_program::op(reg, reg, reg, encoding.funct3, encoding.funct7);
}

uint32_t
filler_instruction_word(uint32_t index)
{
  const auto rd =
      static_cast<uint8_t>(kFillerDstBase + (index % kFillerDstSize));
  return test_program::op(rd, kSrcLargeReg, kSrcSmallReg, 0, 0x00);
}

// Materializes a 32-bit constant into `reg` as lui+addi (or a bare addi when
// the value fits the 12-bit immediate). Prologue only: executed once, so its
// own fusion and cost are absorbed by the fit's intercept.
void
emit_load_immediate(std::vector<uint32_t> &words, uint8_t reg, int32_t value)
{
  if (value >= -2048 && value <= 2047) {
    words.push_back(test_program::addi(reg, 0, value));
    return;
  }
  const int32_t lo =
      static_cast<int32_t>(static_cast<uint32_t>(value) << 20) >> 20;
  const uint32_t hi = static_cast<uint32_t>(value) - static_cast<uint32_t>(lo);
  words.push_back(test_program::u_type(0x37U, reg, hi));
  words.push_back(test_program::addi(reg, reg, lo));
}

void
emit_prologue(std::vector<uint32_t> &words, WorkingSet working_set,
              uint32_t trip_count)
{
  emit_load_immediate(words, kLoopCounterReg, static_cast<int32_t>(trip_count));
  emit_load_immediate(words, kSrcLargeReg, kSrcLargeValue);
  emit_load_immediate(words, kSrcSmallReg, kSrcSmallValue);
  emit_load_immediate(words, kStoreValueReg, 0x0BADF00D);
  // A pointer chase starts at node 0; every other context wants a full-width
  // value in the chain register.
  emit_load_immediate(words, kChainReg,
                      working_set == WorkingSet::kPointerChase
                          ? 0
                          : kSrcLargeValue);
  emit_load_immediate(words, kOneReg, 1);
  for (uint32_t i = 0; i < kFillerDstSize; ++i) {
    emit_load_immediate(words, static_cast<uint8_t>(kFillerDstBase + i), 1);
  }
  for (uint32_t i = 0; i < kDataBankSize; ++i) {
    emit_load_immediate(words, static_cast<uint8_t>(kDataBankBase + i),
                        static_cast<int32_t>(0x100 + i));
  }
  for (uint32_t i = 0; i < kLoadDstSize; ++i) {
    emit_load_immediate(words, static_cast<uint8_t>(kLoadDstBase + i), 0);
  }
  for (uint32_t i = 0; i < kAddressBankSize; ++i) {
    emit_load_immediate(words, static_cast<uint8_t>(kAddressBankBase + i),
                        static_cast<int32_t>(
                            address_base_value(working_set, i)));
  }
  // The last instruction emitted is an addi from emit_load_immediate. An
  // R-type filler placed after it cannot fuse (the ALU-ALU rule pairs addi
  // only with addi or sll), which is what pins the prologue/body boundary.
}

std::expected<uint64_t, KernelError>
scheduled_gas_for_code(std::span<const uint8_t> code)
{
  const auto program = ThreadedProgram::create(code);
  if (!program.has_value()) {
    return std::unexpected(KernelError::kUndecodable);
  }
  uint64_t total = 0;
  for (size_t i = 0; i < program->instruction_count(); ++i) {
    const auto instruction = program->instruction_at(i);
    if (!instruction.has_value()) {
      return std::unexpected(KernelError::kUndecodable);
    }
    total += instruction->gas;
  }
  return total;
}

std::string
kernel_error_name(KernelError error)
{
  switch (error) {
  case KernelError::kUnsupportedTarget:
    return "no body-slot encoding for this opcode";
  case KernelError::kBodyTooSmall:
    return "body too small for the fixed boundaries";
  case KernelError::kZeroTripCount:
    return "trip count is zero";
  case KernelError::kTooManyTargets:
    return "too many targets to keep them non-adjacent";
  case KernelError::kCodeTooLarge:
    return "generated code exceeds kMaxCodeBytes";
  case KernelError::kUndecodable:
    return "generated code did not decode";
  }
  return "unknown";
}

bool
is_measurable_target(Op op)
{
  switch (op) {
  // Excluded by construction: JAL/JALR need a per-slot rewritten target,
  // ECALL/EBREAK/UNIMP end the run, and FENCE is not a body-shaped op.
  case Op::kJal:
  case Op::kJalr:
  case Op::kFence:
  case Op::kEcall:
  case Op::kEbreak:
  case Op::kUnimp:
  case Op::kCount:
    return false;
  default:
    return true;
  }
}

bool
is_memory_target(Op op)
{
  return is_load(op) || is_store(op);
}

bool
is_divide_target(Op op)
{
  return op == Op::kDiv || op == Op::kDivu || op == Op::kRem || op == Op::kRemu;
}

bool
is_branch_target(Op op)
{
  switch (op) {
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
    return true;
  default:
    return false;
  }
}

std::string
op_name(Op op)
{
  const auto index = static_cast<size_t>(op);
  if (index >= kInstructionGasSchedule.size()) {
    return "UNKNOWN";
  }
  const char8_t *name = kInstructionGasSchedule[index].name;
  return {reinterpret_cast<const char *>(name)};
}

std::string
context_name(OperandContext context)
{
  return context == OperandContext::kChain ? "chain" : "independent";
}

std::string
working_set_name(WorkingSet working_set)
{
  switch (working_set) {
  case WorkingSet::kL1Region:
    return "l1_4k";
  case WorkingSet::kFullRange:
    return "full_64k";
  case WorkingSet::kPointerChase:
    return "chase_64k";
  default:
    return "none";
  }
}

std::string
magnitude_name(OperandMagnitude magnitude)
{
  switch (magnitude) {
  case OperandMagnitude::kSmall:
    return "small";
  case OperandMagnitude::kLarge:
    return "large";
  default:
    return "default";
  }
}

uint64_t
scheduled_gas_for(Op op)
{
  const auto index = static_cast<size_t>(op);
  if (index >= kInstructionGasSchedule.size()) {
    return 0;
  }
  return kInstructionGasSchedule[index].cost;
}

std::expected<Kernel, KernelError>
build_kernel(const KernelSpec &spec)
{
  if (!is_measurable_target(spec.target)) {
    return std::unexpected(KernelError::kUnsupportedTarget);
  }
  if (spec.body_slots < 8) {
    return std::unexpected(KernelError::kBodyTooSmall);
  }
  if (spec.trip_count == 0) {
    return std::unexpected(KernelError::kZeroTripCount);
  }
  // Slot 0 and the last slot always hold the filler: that pins the two body
  // boundaries (prologue -> body, body -> loop control) to a fixed instruction
  // pair for every kernel in a sweep, so no boundary fusion can appear or
  // disappear as target_slots varies. Targets therefore live in the interior,
  // and capping them at half the interior keeps any two targets non-adjacent.
  const uint32_t interior_slots = spec.body_slots - 2;
  if (spec.target_slots > interior_slots / 2) {
    return std::unexpected(KernelError::kTooManyTargets);
  }

  std::vector<uint32_t> prologue;
  emit_prologue(prologue, spec.working_set, spec.trip_count);

  std::vector<uint32_t> body(spec.body_slots);
  for (uint32_t slot = 0; slot < spec.body_slots; ++slot) {
    body[slot] = filler_instruction_word(slot);
  }
  std::vector<uint32_t> target_slot_indices;
  target_slot_indices.reserve(spec.target_slots);
  if (spec.target_slots > 0) {
    // Even (Bresenham) placement across the interior, so targets are spread
    // rather than clustered: a clustered prefix would give the interior of the
    // cluster target-to-target adjacency and the remainder filler-to-filler,
    // making the fusion exposure a function of target_slots -- the exact
    // confound the fixed-boundary design exists to remove.
    for (uint32_t i = 0; i < interior_slots; ++i) {
      const uint32_t before = (i * spec.target_slots) / interior_slots;
      const uint32_t after = ((i + 1) * spec.target_slots) / interior_slots;
      if (after != before) {
        // Rotate the whole evenly-spaced set. A cyclic rotation of a cyclically
        // even set keeps every gap, so targets stay non-adjacent at any phase.
        const uint32_t rotated = (i + spec.placement_phase) % interior_slots;
        target_slot_indices.push_back(rotated + 1);
      }
    }
  }
  std::ranges::sort(target_slot_indices);
  for (uint32_t ordinal = 0; ordinal < target_slot_indices.size(); ++ordinal) {
    body[target_slot_indices[ordinal]] = target_word(spec, ordinal);
  }

  std::vector<uint32_t> words = prologue;
  const auto body_start_index = static_cast<uint32_t>(words.size());
  words.insert(words.end(), body.begin(), body.end());
  // Loop control: decrement and branch back to the body head. This pair fuses
  // (fuse_alu_branch) into one slot, which is fine -- it is identical in every
  // kernel of a sweep and lands in the fit's intercept.
  words.push_back(test_program::addi(kLoopCounterReg, kLoopCounterReg, -1));
  const int32_t back_edge =
      -static_cast<int32_t>((spec.body_slots + 1) * kInstructionBytes);
  words.push_back(test_program::branch(1, kLoopCounterReg, 0, back_edge));
  words.push_back(0x00000073U); // ecall: the halting host ends the run

  if (words.size() * kInstructionBytes > kMaxCodeBytes) {
    return std::unexpected(KernelError::kCodeTooLarge);
  }

  Kernel kernel;
  kernel.code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(kernel.code, word);
  }
  kernel.memory_bytes = kMaxMemoryBytes;
  kernel.initial_memory.assign(kMaxMemoryBytes, 0);
  if (spec.working_set == WorkingSet::kPointerChase) {
    seed_pointer_chase(kernel.initial_memory);
  }
  kernel.body_start_pc = body_start_index * kInstructionBytes;
  for (const uint32_t slot : target_slot_indices) {
    kernel.target_slot_pcs.push_back((body_start_index + slot) *
                                     kInstructionBytes);
  }

  kernel.instructions_per_iteration = spec.body_slots + 2;
  kernel.dynamic_instructions =
      prologue.size() +
      (uint64_t{spec.trip_count} * kernel.instructions_per_iteration) + 1;

  uint64_t body_gas = 0;
  for (uint32_t slot = 0; slot < spec.body_slots; ++slot) {
    const bool is_target = std::ranges::find(target_slot_indices, slot) !=
                           target_slot_indices.end();
    body_gas += is_target ? scheduled_gas_for(spec.target)
                          : scheduled_gas_for(Op::kAdd);
  }
  const uint64_t loop_gas =
      scheduled_gas_for(Op::kAddi) + scheduled_gas_for(Op::kBne);
  uint64_t prologue_gas = 0;
  for (size_t i = 0; i < prologue.size(); ++i) {
    prologue_gas += kBaseInstructionGas;
  }
  kernel.scheduled_gas = prologue_gas +
                         (uint64_t{spec.trip_count} * (body_gas + loop_gas)) +
                         scheduled_gas_for(Op::kEcall);
  return kernel;
}

} // namespace hogd::rv32::gas_calibration
