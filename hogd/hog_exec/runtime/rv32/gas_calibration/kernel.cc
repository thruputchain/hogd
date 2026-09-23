#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
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
// Guest memory is capped at kDefaultMaxMemoryBytes (64 KiB), so those two
// contexts bracket the entire cache behaviour a guest can reach; there is no
// DRAM-resident case to price.
struct MemoryOperand {
  uint8_t base_reg = 0;
  int32_t offset = 0;
};

MemoryOperand
memory_operand(WorkingSet working_set, uint32_t index, uint32_t width)
{
  if (working_set == WorkingSet::kSameAddress) {
    // Every access to one address. A store has no result to chain through, so
    // this is the closest a single-opcode sweep gets to a dependent store:
    // successive stores contend for the same store-buffer entry instead of
    // draining independently.
    return {.base_reg = kAddressBankBase, .offset = 0};
  }
  const auto base_reg = static_cast<uint8_t>(
      kAddressBankBase +
      (working_set == WorkingSet::kFullRange ? index % kAddressBankSize : 0));
  // Visit each bank at a given offset before advancing to the next line.
  // Advancing both bank and offset on every access aliases their periods:
  // the old pattern repeated after 64 accesses and touched only 4 KiB of
  // cache lines, even though its addresses spanned the guest address space.
  const uint32_t line_index =
      working_set == WorkingSet::kFullRange ? index / kAddressBankSize : index;
  const int32_t span = 4096;
  const auto raw =
      static_cast<int32_t>((line_index * 64U) % static_cast<uint32_t>(span));
  int32_t offset = raw - 2048;
  offset -= offset % static_cast<int32_t>(width); // keep accesses aligned
  return {.base_reg = base_reg, .offset = offset};
}

// The address range a pointer chase can cover with `width`-byte loads, and the
// node stride inside it.
//
// A chase node has to hold the address of the next node, so a narrow load can
// only chase as far as its loaded value reaches -- and for the sign-extending
// loads (LB, LH) only half of that, since a negative value is not an address.
// LW reaches all of guest memory; LHU reaches all of it too; LH half; LBU and
// LB only the first 256 and 128 bytes. The narrow ones therefore measure
// load-use *latency* without cache pressure, which is still the thing a
// single-opcode throughput sweep cannot see.
struct ChaseGeometry {
  uint32_t range = 0;  // addresses live in [0, range)
  uint32_t stride = 0; // bytes between nodes
};

ChaseGeometry
chase_geometry(Op op, uint32_t memory_bytes)
{
  switch (op) {
  case Op::kLw:
    return {.range = memory_bytes, .stride = kChaseStride};
  case Op::kLhu:
    return {.range = std::min<uint32_t>(memory_bytes, 0x10000),
            .stride = kChaseStride};
  case Op::kLh:
    return {.range = std::min<uint32_t>(memory_bytes, 0x8000),
            .stride = kChaseStride};
  case Op::kLbu:
    return {.range = 256, .stride = 4};
  case Op::kLb:
    return {.range = 128, .stride = 4};
  default:
    return {};
  }
}

// Writes a single shuffled cycle through the chase nodes: the value at node i
// is the address of node next(i), stored in `width` bytes. A shuffle rather
// than a fixed stride is the point -- a strided walk is prefetchable, so it
// would measure the prefetcher rather than the load latency the chase exists to
// expose. The permutation is a fixed-seed LCG so two runs walk the same cycle
// and their numbers are comparable.
void
seed_pointer_chase(std::vector<uint8_t> &memory, const ChaseGeometry &geometry,
                   uint32_t width)
{
  if (geometry.stride == 0 || geometry.range > memory.size()) {
    return;
  }
  const uint32_t nodes = geometry.range / geometry.stride;
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
    const uint32_t from = order[i] * geometry.stride;
    const uint32_t to = order[(i + 1) % nodes] * geometry.stride;
    for (uint32_t byte = 0; byte < width; ++byte) {
      memory[from + byte] = static_cast<uint8_t>(to >> (byte * 8));
    }
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
target_word(const KernelSpec &spec, uint32_t index, uint32_t slot)
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
  if (spec.target == Op::kJal) {
    // Like the JALR sweep, land on the next instruction so every body slot
    // executes. JAL is PC-relative: +4 works at every slot without patching.
    uint8_t rd = 0;
    if (spec.context != OperandContext::kNoLink) {
      rd = static_cast<uint8_t>(kDataBankBase + (index % kDataBankSize));
      if (chain) {
        rd = kChainReg;
      }
    }
    return test_program::j_type(rd, kInstructionBytes);
  }
  if (spec.target == Op::kJalr) {
    // A JALR that lands on the instruction after itself: a real indirect
    // control transfer that leaves the retired instruction sequence unchanged,
    // which is what lets it sit in a body slot at all. Same idea as the
    // taken-branch form.
    //
    // The target is rs1 + imm with the low bit cleared. rs1 holds a fixed base
    // (the body's midpoint, staged by the prologue), so the immediate carries
    // the whole per-slot offset: imm = (this slot's pc + 4) - base. A 12-bit
    // signed immediate reaches +/-2048 bytes, and anchoring at the midpoint
    // puts every slot of a 512-instruction body inside that -- which is why
    // the base is the midpoint and not the start.
    //
    // rd rotates over the data bank so the link writes do not serialize.
    const uint8_t rd =
        chain ? kChainReg
              : static_cast<uint8_t>(kDataBankBase + (index % kDataBankSize));
    // Both the slot after this one and the base are offsets from the same
    // origin, so the origin cancels and the immediate is purely relative.
    const auto midpoint = static_cast<int32_t>(spec.body_slots / 2);
    const auto imm =
        static_cast<int32_t>((static_cast<int32_t>(slot) + 1 - midpoint) *
                             static_cast<int32_t>(kInstructionBytes));
    return test_program::jalr(rd, kJalrBaseReg, imm);
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
    // Chain through the dividend: `op x13, x13, one`. Every slot waits on the
    // previous result, which is the arrangement that forces divider latency
    // rather than throughput, and so the one gas has to price.
    //
    // The operands degenerate, and that is worth stating plainly because it
    // looks like a bug. DIV keeps its dividend (x / 1 == x), but REM and REMU
    // go to zero on their first instruction and every later one computes
    // 0 % 1. A reviewer flagged that as understating the worst remainder
    // input, which is the natural reading. It is not what happens.
    //
    // Measured, both ways, on the same host: this shape puts chained REM at
    // 9.8x the baseline instruction -- about 14 cycles, a plausible divider
    // latency. Chaining the divisor instead (`op x13, x28, x13`, which does
    // keep a full-width dividend) drops it to 2.4x, about 3 cycles, which is
    // too fast to be a division at all: `large % large` has quotient 1 and
    // `large % 0` returns the dividend by definition, so that arrangement
    // early-outs and breaks the very dependency it was built to expose.
    //
    // So the degenerate operands cost nothing here: the divider pays its
    // latency regardless, and this is the more conservative of the two
    // measurable shapes. Worst-case pricing wants that. What no single
    // remainder instruction can do is feed itself hard operands indefinitely --
    // `a % x` contracts -- so a sustained hard-operand dependent remainder
    // needs a second instruction to refresh the divisor, making it a pair
    // rather than a substitution. The independent context (`large % 3`, a
    // genuine full-width remainder every time) is measured alongside this one
    // and the schedule prices whichever is worse.
    //
    // Nothing here can trap: RISC-V defines division by zero as -1, remainder
    // by zero as the dividend, and signed overflow as INT_MIN.
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

// The two words of one pair substitution.
//
// Both kinds chain through kChainReg -- pair k+1's store consumes pair k's
// load -- so the register dependence is identical and the only difference is
// whether the load's address is the one the store just wrote. That is what
// makes the difference between them attributable to store-to-load forwarding
// and to nothing else.
//
// Addresses: the base register holds kL1RegionBase (2048) and a 12-bit signed
// offset reaches [0, 4095]. Forwarding uses one address for both halves;
// distinct sends stores to the upper 2 KiB and loads to the lower, which never
// overlap, so no load can read a line a store recently touched.
std::array<uint32_t, 2>
pair_words(const KernelSpec &spec, uint32_t index)
{
  if (spec.pair == PairKind::kAuipcJalr) {
    // `auipc rX, 0` puts its own pc in rX; the jalr then targets rX + 8, which
    // is the instruction after the jalr. Control lands where it would have
    // anyway, so the retired sequence is unchanged, and the pair is the exact
    // shape fuse_auipc_jalr matches.
    const auto link =
        static_cast<uint8_t>(kDataBankBase + (index % kDataBankSize));
    return {test_program::u_type(0x17U, kChainReg, 0),
            test_program::jalr(link, kChainReg, 2 * kInstructionBytes)};
  }
  constexpr uint8_t kWordFunct3 = 2; // sw / lw
  const auto store_offset =
      static_cast<int32_t>((index * 64U) % 2048U); // aligned, line-strided
  const int32_t load_offset = spec.pair == PairKind::kStoreLoadForward
                                  ? store_offset
                                  : store_offset - 2048;
  return {test_program::store(kAddressBankBase, kChainReg, store_offset,
                              kWordFunct3),
          test_program::load(kChainReg, kWordFunct3, kAddressBankBase,
                             load_offset)};
}

} // namespace

bool
pair_expects_fusion(PairKind pair)
{
  return pair == PairKind::kAuipcJalr;
}

uint32_t
slots_per_target(const KernelSpec &spec)
{
  return spec.pair == PairKind::kNone ? 1 : 2;
}

uint32_t
max_target_slots(uint32_t body_slots, const KernelSpec &spec)
{
  if (body_slots < 8) {
    return 0;
  }
  const uint32_t per_target = slots_per_target(spec);
  // Slot 0 and the last slot stay filler; a pair's tail must also fit inside
  // that interior, so the head can sit one slot lower than a single target's.
  const uint32_t placeable = (body_slots - 2) - (per_target - 1);
  if (spec.saturated) {
    // Every interior slot a substitution can start at without landing on the
    // previous one. Adjacency is the point, so unlike the sweep below there is
    // no filler gap to keep -- but a pair still occupies two slots, and a head
    // at every slot would make each pair's first instruction overwrite the
    // previous pair's second. The body would come out as a run of stores with
    // one trailing load, audit clean (sw;sw is not a fusion rule), and be
    // reported as the cost of a pair. Dividing by per_target tiles them
    // instead: sw lw sw lw, which is what "a program made of this pair" means.
    return placeable / per_target;
  }
  // One filler between substitutions, so the period is per_target + 1. Deriving
  // the cap from the same span the placement walks is what keeps the minimum
  // gap at or above that period.
  return placeable / (per_target + 1);
}

std::pair<Op, Op>
pair_components(PairKind pair)
{
  switch (pair) {
  case PairKind::kStoreLoadForward:
  case PairKind::kStoreLoadDistinct:
    return {Op::kSw, Op::kLw};
  case PairKind::kAuipcJalr:
    return {Op::kAuipc, Op::kJalr};
  default:
    return {Op::kAdd, Op::kAdd};
  }
}

std::string
pair_name(PairKind pair)
{
  switch (pair) {
  case PairKind::kStoreLoadForward:
    return "sw;lw forward";
  case PairKind::kStoreLoadDistinct:
    return "sw;lw distinct";
  case PairKind::kAuipcJalr:
    return "auipc;jalr";
  default:
    return "none";
  }
}

std::expected<uint32_t, KernelError>
canonical_instruction_word(Op op, uint8_t reg)
{
  // Keep the fingerprint's canonical fusion corpus unchanged when adding a
  // timing kernel. Direct jumps are measured separately from that corpus.
  if (!is_measurable_target(op) || op == Op::kJal) {
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
  // The JALR base, always a two-word lui+addi so its length does not depend on
  // the value, and always last so a caller can patch it at a known index once
  // it knows where the body landed (build_kernel does). It also ends the
  // prologue on an `addi`: an R-type filler placed after it cannot fuse, since
  // the ALU-ALU rule pairs addi only with addi or sll, which is what pins the
  // prologue/body boundary.
  words.push_back(test_program::u_type(0x37U, kJalrBaseReg, 0));
  words.push_back(test_program::addi(kJalrBaseReg, kJalrBaseReg, 0));
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
  case KernelError::kTargetImmediateOutOfRange:
    return "target immediate out of range";
  case KernelError::kCodeTooLarge:
    return "generated code exceeds kDefaultMaxCodeBytes";
  case KernelError::kUndecodable:
    return "generated code did not decode";
  }
  return "unknown";
}

bool
is_measurable_target(Op op)
{
  switch (op) {
  // ECALL/EBREAK/UNIMP end the run, and FENCE is not a body-shaped op.
  // JAL and JALR land on the next slot -- see target_word.
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
is_load_target(Op op)
{
  return is_load(op);
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
  if (context == OperandContext::kTargetChase) {
    return "target_chase";
  }
  if (context == OperandContext::kNoLink) {
    return "no_link";
  }
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
    return "chase";
  case WorkingSet::kSameAddress:
    return "same_addr";
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
  if (spec.pair == PairKind::kNone && !is_measurable_target(spec.target)) {
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
  const uint32_t per_target = slots_per_target(spec);
  if (spec.target_slots > max_target_slots(spec.body_slots, spec)) {
    return std::unexpected(KernelError::kTooManyTargets);
  }
  const uint32_t placeable_slots = interior_slots - (per_target - 1);

  std::vector<uint32_t> prologue;
  emit_prologue(prologue, spec.working_set, spec.trip_count);

  // JALR's target is rs1 + imm with rs1 fixed at the body midpoint, so the
  // immediate carries the whole per-slot offset and its 12-bit signed field
  // decides how large a body this target can fill. The encoder masks rather
  // than refuses, so without this the kernel would build, decode and run --
  // jumping somewhere inside the body instead of to the next slot, exactly the
  // silent truncation the loop back edge used to suffer.
  if (spec.target == Op::kJalr && spec.body_slots >= 4) {
    const auto midpoint = static_cast<int32_t>(spec.body_slots / 2);
    const auto offset = [&](uint32_t slot) {
      return (static_cast<int32_t>(slot) + 1 - midpoint) *
             static_cast<int32_t>(kInstructionBytes);
    };
    constexpr int32_t kJalrImmediateLimit = 1 << 11;
    const int32_t lowest = offset(1);
    const int32_t highest = offset(spec.body_slots - 2);
    if (lowest < -kJalrImmediateLimit || highest >= kJalrImmediateLimit) {
      return std::unexpected(KernelError::kTargetImmediateOutOfRange);
    }
  }

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
    for (uint32_t i = 0; i < placeable_slots; ++i) {
      const uint32_t before = (i * spec.target_slots) / placeable_slots;
      const uint32_t after = ((i + 1) * spec.target_slots) / placeable_slots;
      if (after != before) {
        // Rotate the whole evenly-spaced set. A cyclic rotation of a cyclically
        // even set keeps every gap, so targets stay non-adjacent at any phase.
        const uint32_t rotated = (i + spec.placement_phase) % placeable_slots;
        target_slot_indices.push_back(rotated + 1);
      }
    }
  }
  std::ranges::sort(target_slot_indices);
  for (uint32_t ordinal = 0; ordinal < target_slot_indices.size(); ++ordinal) {
    const uint32_t slot = target_slot_indices[ordinal];
    if (spec.pair == PairKind::kNone) {
      body[slot] = target_word(spec, ordinal, slot);
      continue;
    }
    const std::array<uint32_t, 2> words = pair_words(spec, ordinal);
    body[slot] = words[0];
    body[slot + 1] = words[1];
  }

  std::vector<uint32_t> words = prologue;
  const auto body_start_index = static_cast<uint32_t>(words.size());
  // Patch the JALR base now that the body's address is known; emit_prologue
  // left the two words that materialize it at the end of the prologue.
  // Anchoring at the body's midpoint is what keeps every slot's immediate
  // inside JALR's 12-bit signed reach.
  {
    const uint32_t base_address =
        (body_start_index + (spec.body_slots / 2)) * kInstructionBytes;
    const int32_t lo = static_cast<int32_t>(base_address << 20) >> 20;
    const uint32_t hi = base_address - static_cast<uint32_t>(lo);
    words[body_start_index - 2] = test_program::u_type(0x37U, kJalrBaseReg, hi);
    words[body_start_index - 1] =
        test_program::addi(kJalrBaseReg, kJalrBaseReg, lo);
  }
  words.insert(words.end(), body.begin(), body.end());
  // Loop control: decrement, then a forward branch that skips a JAL back to
  // the body head.
  //
  // Not a backwards branch: a B-type immediate is 13 bits signed, reaching
  // only 1024 instructions, and the encoder truncates rather than refuses --
  // the kernel jumps into the middle of itself and still halts cleanly. JAL's
  // 21 bits cover anything kDefaultMaxCodeBytes allows, and the branch skipping
  // it only ever jumps two instructions.
  words.push_back(test_program::addi(kLoopCounterReg, kLoopCounterReg, -1));
  // beq counter, x0, +8 -- when the counter hits zero, skip the JAL and fall
  // into the ecall.
  words.push_back(
      test_program::branch(0, kLoopCounterReg, 0,
                           2 * static_cast<int32_t>(kInstructionBytes)));
  const int32_t back_edge =
      -static_cast<int32_t>((spec.body_slots + 2) * kInstructionBytes);
  words.push_back(test_program::j_type(0, back_edge));
  words.push_back(0x00000073U); // ecall: the halting host ends the run

  if (words.size() * kInstructionBytes > kDefaultMaxCodeBytes) {
    return std::unexpected(KernelError::kCodeTooLarge);
  }

  Kernel kernel;
  kernel.code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(kernel.code, word);
  }
  kernel.memory_bytes = kDefaultMaxMemoryBytes;
  kernel.initial_memory.assign(kDefaultMaxMemoryBytes, 0);
  if (spec.working_set == WorkingSet::kPointerChase) {
    seed_pointer_chase(kernel.initial_memory,
                       chase_geometry(spec.target, kernel.memory_bytes),
                       memory_access_width(spec.target));
  }
  kernel.body_start_pc = body_start_index * kInstructionBytes;
  kernel.target_units = static_cast<uint32_t>(target_slot_indices.size());
  for (const uint32_t slot : target_slot_indices) {
    // Record every slot a substitution occupies, so the audit checks both
    // halves of a pair rather than only its head.
    for (uint32_t offset = 0; offset < per_target; ++offset) {
      kernel.target_slot_pcs.push_back((body_start_index + slot + offset) *
                                       kInstructionBytes);
    }
  }

  // Steady state: the body, the decrement, the skip branch, and the JAL back.
  kernel.instructions_per_iteration = spec.body_slots + 3;
  // The last iteration takes the skip branch instead of the JAL, so the JAL
  // retires trip_count - 1 times rather than trip_count.
  kernel.dynamic_instructions =
      prologue.size() + (uint64_t{spec.trip_count} * (spec.body_slots + 2)) +
      (uint64_t{spec.trip_count} - 1) + 1;

  // Charge the body by decoding it with the production decoder rather than by
  // reasoning about which slot holds what. The slot-walking version silently
  // assumed one instruction per target, which stopped being true the moment a
  // substitution could place a pair -- and a gas total that disagrees with what
  // the engine charges would break the test that pins them equal.
  const std::vector<uint8_t> body_code = test_program::code_from_words(body);
  const auto body_gas = scheduled_gas_for_code(body_code);
  if (!body_gas.has_value()) {
    return std::unexpected(body_gas.error());
  }
  const std::vector<uint8_t> prologue_code =
      test_program::code_from_words(prologue);
  const auto prologue_gas = scheduled_gas_for_code(prologue_code);
  if (!prologue_gas.has_value()) {
    return std::unexpected(prologue_gas.error());
  }
  // Charged per iteration: the decrement and the skip branch. The JAL back is
  // charged separately because the final iteration takes the branch instead,
  // so it executes one time fewer than the loop runs.
  const uint64_t loop_gas =
      scheduled_gas_for(Op::kAddi) + scheduled_gas_for(Op::kBeq);
  kernel.scheduled_gas =
      *prologue_gas + (uint64_t{spec.trip_count} * (*body_gas + loop_gas)) +
      ((uint64_t{spec.trip_count} - 1) * scheduled_gas_for(Op::kJal)) +
      scheduled_gas_for(Op::kEcall);
  return kernel;
}

std::string
jump_context_name(const JumpSpec &spec)
{
  const char *layout = "shuffled";
  if (spec.layout == JumpLayout::kSequential) {
    layout = "sequential";
  }
  else if (spec.layout == JumpLayout::kReverse) {
    layout = "reverse";
  }
  std::string mode = context_name(spec.context);
  if (spec.context == OperandContext::kChain) {
    mode = "link_chain";
  }
  else if (spec.target == Op::kJalr &&
           spec.context != OperandContext::kTargetChase) {
    mode = "fixed_base_" + mode;
  }
  return mode + "/" + layout + "/n" + std::to_string(spec.nodes) + "/stride" +
         std::to_string(spec.stride_words) + "/seed" +
         std::to_string(spec.seed);
}

std::expected<JumpKernel, KernelError>
build_jump_kernel(const JumpSpec &spec)
{
  if (spec.target != Op::kJal && spec.target != Op::kJalr) {
    return std::unexpected(KernelError::kUnsupportedTarget);
  }
  const bool chase = spec.context == OperandContext::kTargetChase;
  if ((chase || spec.context == OperandContext::kChain) &&
      spec.target != Op::kJalr) {
    return std::unexpected(KernelError::kUnsupportedTarget);
  }
  if (spec.nodes < 2 || spec.stride_words == 0) {
    return std::unexpected(KernelError::kBodyTooSmall);
  }
  if (spec.trip_count == 0) {
    return std::unexpected(KernelError::kZeroTripCount);
  }
  // Leave space for the prologue, dispatcher and loop tail. Check before any
  // multiplication/allocation; the encoder also checks each immediate below.
  const uint64_t body_words = uint64_t{spec.nodes} * spec.stride_words;
  if (body_words + 16 > kDefaultMaxCodeBytes / kInstructionBytes ||
      (chase && (uint64_t{spec.nodes} + 1) * 8 > kDefaultMaxMemoryBytes)) {
    return std::unexpected(KernelError::kCodeTooLarge);
  }
  auto permutation = [&](uint32_t seed) {
    std::vector<uint32_t> order(spec.nodes);
    for (uint32_t i = 0; i < spec.nodes; ++i) {
      order[i] = i;
    }
    if (spec.layout == JumpLayout::kReverse) {
      std::reverse(order.begin() + 1, order.end());
    }
    else if (spec.layout == JumpLayout::kShuffled) {
      uint64_t state = seed;
      for (uint32_t i = spec.nodes - 1; i > 1; --i) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto j = 1 + static_cast<uint32_t>((state >> 32) % i);
        std::swap(order[i], order[j]);
      }
    }
    return order;
  };
  const auto order = permutation(spec.seed);
  std::vector<uint32_t> words;
  emit_load_immediate(words, kLoopCounterReg,
                      static_cast<int32_t>(spec.trip_count));
  JumpKernel out;
  Kernel &kernel = out.kernel;
  out.loop_pc = static_cast<uint32_t>(words.size()) * kInstructionBytes;
  const auto setup = static_cast<uint32_t>(words.size());
  // Fixed-size register initialization, executed once per traversal. This
  // resets a link chain or linked-table cursor, not after every target jump.
  words.resize(words.size() + 2);
  const uint32_t hub_pc =
      static_cast<uint32_t>(words.size()) * kInstructionBytes;
  if (chase) {
    words.push_back(test_program::load(kChainReg, 2, kChainReg, 0));
    words.push_back(test_program::load(kDataBankBase, 2, kChainReg, 4));
    words.push_back(test_program::jalr(0, kDataBankBase, 0));
  }
  const uint32_t body_pc =
      static_cast<uint32_t>(words.size()) * kInstructionBytes;
  kernel.body_start_pc = body_pc;
  words.resize(words.size() + static_cast<size_t>(body_words), 0x00100073U);
  const uint32_t tail_pc =
      static_cast<uint32_t>(words.size()) * kInstructionBytes;
  uint32_t base = body_pc + (static_cast<uint32_t>(body_words) * 2);
  if (chase) {
    base = spec.nodes * 8;
  }
  const uint8_t base_reg = chase || spec.context == OperandContext::kChain
                               ? kChainReg
                               : kJalrBaseReg;
  const int32_t lo = static_cast<int32_t>(base << 20) >> 20;
  words[setup] =
      test_program::u_type(0x37U, base_reg, base - static_cast<uint32_t>(lo));
  words[setup + 1] = test_program::addi(base_reg, base_reg, lo);
  out.iteration_pcs = {out.loop_pc, out.loop_pc + kInstructionBytes};
  const auto pc_for = [&](uint32_t node) {
    return body_pc + (node * spec.stride_words * kInstructionBytes);
  };
  uint32_t previous_link = base;
  for (uint32_t i = 0; i < spec.nodes; ++i) {
    const uint32_t pc = pc_for(order[i]);
    const uint32_t next = i + 1 == spec.nodes ? tail_pc : pc_for(order[i + 1]);
    uint32_t word = 0;
    if (chase) {
      word = test_program::j_type(0, static_cast<int32_t>(hub_pc) -
                                         static_cast<int32_t>(pc));
      out.iteration_pcs.insert(out.iteration_pcs.end(),
                               {hub_pc, hub_pc + 4, hub_pc + 8});
      kernel.target_slot_pcs.push_back(hub_pc + 8);
    }
    else if (spec.target == Op::kJal) {
      const uint8_t rd =
          spec.context == OperandContext::kNoLink
              ? 0
              : static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
      word = test_program::j_type(rd, static_cast<int32_t>(next) -
                                          static_cast<int32_t>(pc));
    }
    else {
      // In link_chain, rs1 is exactly the register the preceding JALR wrote.
      // The immediate compensates for that preceding PC+4, not a fixed base.
      const uint32_t input =
          spec.context == OperandContext::kChain ? previous_link : base;
      const int64_t offset = int64_t{next} - input;
      if (offset < -2048 || offset > 2047) {
        return std::unexpected(KernelError::kTargetImmediateOutOfRange);
      }
      uint8_t rd = 0;
      if (spec.context == OperandContext::kChain) {
        rd = kChainReg;
      }
      else if (spec.context != OperandContext::kNoLink) {
        rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
      }
      word = test_program::jalr(rd, base_reg, static_cast<int32_t>(offset));
      previous_link = pc + kInstructionBytes;
    }
    words[pc / kInstructionBytes] = word;
    out.iteration_pcs.push_back(pc);
    if (!chase) {
      kernel.target_slot_pcs.push_back(pc);
    }
  }
  if (chase) {
    // Each visit to the SAME jalr PC loads a different target. Table links and
    // landing pads have independent permutations. Include the terminal visit.
    const auto table_order = permutation(spec.seed ^ 0xA511E9B3U);
    kernel.initial_memory.resize((spec.nodes + 1) * 8);
    const auto put = [&](uint32_t address, uint32_t value) {
      for (uint32_t byte = 0; byte < 4; ++byte) {
        kernel.initial_memory[address + byte] =
            static_cast<uint8_t>(value >> (8 * byte));
      }
    };
    put(spec.nodes * 8, table_order.front() * 8);
    put((spec.nodes * 8) + 4, tail_pc);
    for (uint32_t i = 0; i < spec.nodes; ++i) {
      put(table_order[i] * 8,
          i + 1 == spec.nodes ? spec.nodes * 8 : table_order[i + 1] * 8);
      put((table_order[i] * 8) + 4, pc_for(order[i]));
    }
    out.iteration_pcs.insert(out.iteration_pcs.end(),
                             {hub_pc, hub_pc + 4, hub_pc + 8});
    kernel.target_slot_pcs.push_back(hub_pc + 8);
  }
  words.push_back(test_program::addi(kLoopCounterReg, kLoopCounterReg, -1));
  words.push_back(test_program::branch(0, kLoopCounterReg, 0, 8));
  words.push_back(
      test_program::j_type(0, static_cast<int32_t>(out.loop_pc) -
                                  static_cast<int32_t>(tail_pc + 8)));
  out.iteration_pcs.insert(out.iteration_pcs.end(),
                           {tail_pc, tail_pc + 4, tail_pc + 8});
  out.exit_pc = static_cast<uint32_t>(words.size()) * kInstructionBytes;
  words.push_back(0x00000073U);
  for (const uint32_t word : words) {
    append_u32(kernel.code, word);
  }
  uint64_t prefix_gas = 0;
  uint64_t iteration_gas = 0;
  const auto gas_at = [&](uint32_t pc) {
    return decode_instruction(words[pc / kInstructionBytes])
        .instruction.gas_cost;
  };
  for (uint32_t pc = 0; pc < out.loop_pc; pc += kInstructionBytes) {
    prefix_gas += gas_at(pc);
  }
  for (uint32_t pc : out.iteration_pcs) {
    iteration_gas += gas_at(pc);
  }
  kernel.target_units = static_cast<uint32_t>(kernel.target_slot_pcs.size());
  kernel.memory_bytes =
      std::max<uint32_t>(64,
                         static_cast<uint32_t>(kernel.initial_memory.size()));
  kernel.instructions_per_iteration = out.iteration_pcs.size();
  // Last traversal skips its back edge and executes the terminal ecall.
  kernel.dynamic_instructions =
      (out.loop_pc / kInstructionBytes) +
      (uint64_t{spec.trip_count} * out.iteration_pcs.size());
  kernel.scheduled_gas = prefix_gas +
                         (uint64_t{spec.trip_count} * iteration_gas) -
                         gas_at(tail_pc + 8) + gas_at(out.exit_pc);
  return out;
}

} // namespace hogd::rv32::gas_calibration
