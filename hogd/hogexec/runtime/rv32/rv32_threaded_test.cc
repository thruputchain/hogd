#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/musttail.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/test_corpus.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"
#include "hogd/hogexec/runtime/rv32/threaded_program.h"

namespace hogd::rv32 {

struct FusionCoverage {
  size_t lui_addi = 0;
  size_t auipc_jalr = 0;
  size_t load_rr_rs1 = 0;
  size_t load_rr_rs2 = 0;
  size_t load_ri = 0;
  size_t alu_alu_addi_addi = 0;
  size_t alu_alu_addi_sll = 0;
  size_t alu_alu_srl_addi = 0;
  size_t alu_alu_sll_or = 0;
  size_t alu_store = 0;
  size_t alu_load = 0;
  size_t store_alu = 0;
  size_t load_store = 0;
  size_t alu_branch = 0;
};

struct ThreadedMachinePreInstantiationTestPeer {
  static FusionCoverage
  fusion_coverage(const ThreadedMachinePreInstantiation &program)
  {
    FusionCoverage coverage;
    for (const Slot &slot : program.exec_) {
      const Handler handler = slot.handler;
      if (handler == &op_lui_addi<true>) {
        ++coverage.lui_addi;
      }
      if (handler == &op_auipc_jalr<true>) {
        ++coverage.auipc_jalr;
      }
      if (handler == &op_load_rr<true, 4, false, true, add_op>) {
        ++coverage.load_rr_rs1;
      }
      if (handler == &op_load_rr<true, 4, false, false, add_op>) {
        ++coverage.load_rr_rs2;
      }
      if (handler == &op_load_ri<true, 4, false, add_op>) {
        ++coverage.load_ri;
      }
      if (handler == alu_alu_handler<true>(Op::kAddi, Op::kAddi)) {
        ++coverage.alu_alu_addi_addi;
      }
      if (handler == alu_alu_handler<true>(Op::kAddi, Op::kSll)) {
        ++coverage.alu_alu_addi_sll;
      }
      if (handler == alu_alu_handler<true>(Op::kSrl, Op::kAddi)) {
        ++coverage.alu_alu_srl_addi;
      }
      if (handler == alu_alu_handler<true>(Op::kSll, Op::kOr)) {
        ++coverage.alu_alu_sll_or;
      }
      if (handler == alu_store_handler<true>(Op::kAddi, Op::kSw)) {
        ++coverage.alu_store;
      }
      if (handler == alu_load_handler<true>(Op::kAddi, Op::kLw)) {
        ++coverage.alu_load;
      }
      if (handler == store_alu_handler<true>(Op::kSw, Op::kAddi)) {
        ++coverage.store_alu;
      }
      if (handler == &op_load_store<true, 4, false, 4>) {
        ++coverage.load_store;
      }
      if (handler == alu_branch_handler<true>(Op::kAddi, Op::kBne)) {
        ++coverage.alu_branch;
      }
    }
    return coverage;
  }
};

namespace {

// Writes 4 bytes at a fixed offset through the view, then returns a
// configurable status. Used to prove host guest-memory writes persist (no
// per-ecall rollback).
struct ThreadedMemoryHost : Host {
  enum class Outcome { kContinue, kOutOfGas };
  explicit ThreadedMemoryHost(Outcome outcome) : outcome_(outcome) {}
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    const std::array<uint8_t, 4> data = {0xaa, 0xbb, 0xcc, 0xdd};
    wrote = view.write_memory(4, data);
    return outcome_ == Outcome::kContinue ? HostResult::continue_with(0)
                                          : HostResult::out_of_gas();
  }
  Outcome outcome_;
  bool wrote = false;
};

// Build a byte image (little-endian) from instruction words.
std::vector<uint8_t>
words_to_bytes(std::span<const uint32_t> words)
{
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  return bytes;
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST(Rv32Threaded, decodeMatchesCanonicalOverCorpus)
{
  auto check = [](const std::vector<uint32_t> &words, uint64_t seed,
                  const char *kind) {
    const std::vector<uint8_t> bytes = words_to_bytes(words);

    const auto canonical = Program::create(bytes);
    ASSERT_TRUE(canonical.has_value()) << kind << " seed " << seed;
    const auto threaded = ThreadedProgram::create(bytes);
    ASSERT_TRUE(threaded.has_value()) << kind << " seed " << seed;

    ASSERT_EQ(threaded->instruction_count(), canonical->instruction_count());
    for (size_t i = 0; i < words.size(); ++i) {
      const DecodedInstruction &d = canonical->instruction_at_aligned_pc(
          static_cast<uint32_t>(i) * kInstructionBytes);
      auto decoded = threaded->instruction_at(i);
      ASSERT_TRUE(decoded.has_value());
      const Instr &t = *decoded;
      EXPECT_EQ(t.op, d.op) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rd, d.rd) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rs1, d.rs1) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rs2, d.rs2) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.imm, d.immediate)
          << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.gas, d.gas_cost)
          << kind << " seed " << seed << " instr " << i;
    }
  };

  // Both generators: register-only covers SLLI/SRLI/SRAI (funct3 1/5) and the
  // full ALU/branch/jal surface; memory adds loads/stores. Together they
  // exercise every decode path the corpus can reach.
  for (uint64_t seed = 0; seed < 16; ++seed) {
    check(test_corpus::generate_register_only_program(seed, /*body_len=*/48),
          seed, "reg");
    check(test_corpus::generate_memory_program(seed, /*body_len=*/48), seed,
          "mem");
  }
}

TEST(Rv32Threaded, decodeRejectsLikeCanonical)
{
  // Decode-validation lives in ThreadedMachinePreInstantiation::create (the
  // production gate): ThreadedProgram now just holds the byte span and decodes
  // lazily. Rejection must still match canonical Machine over the same inputs.
  // These inputs fail program decode, which create() reports before it looks
  // at the memory arguments, so the memory image here is irrelevant.
  EXPECT_EQ(ThreadedMachinePreInstantiation::create(
                std::vector<uint8_t>{0x13, 0x00, 0x00}, /*memory=*/{},
                /*memory_start=*/0, /*total_memory_size=*/256, /*trace=*/false)
                .error(),
            ProgramError::kCodeSizeNotAligned);
  const std::vector<uint8_t> compressed = {0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(ThreadedMachinePreInstantiation::create(compressed, /*memory=*/{},
                                                    /*memory_start=*/0,
                                                    /*total_memory_size=*/256,
                                                    /*trace=*/false)
                .error(),
            ProgramError::kUnsupportedCompressedInstruction);
  std::vector<uint8_t> csr(4);
  store_u32_le(csr, 0, 0x00001073U);
  EXPECT_EQ(ThreadedMachinePreInstantiation::create(csr, /*memory=*/{},
                                                    /*memory_start=*/0,
                                                    /*total_memory_size=*/256,
                                                    /*trace=*/false)
                .error(),
            ProgramError::kUnsupportedSystemInstruction);
}

TEST(Rv32Threaded, createRejectsRunGasOverflow)
{
  using test_program::op;
  constexpr size_t kOverflowingDivCount = (0xFFFFU / kDivInstructionGas) + 1;
  const std::vector<uint32_t> words(kOverflowingDivCount,
                                    op(3, 1, 2, 4, 0x01)); // div x3,x1,x2
  const std::vector<uint8_t> bytes = words_to_bytes(words);

  ASSERT_TRUE(Program::create(bytes).has_value());
  auto threaded =
      ThreadedMachinePreInstantiation::create(bytes, /*memory=*/{},
                                              /*memory_start=*/0,
                                              /*total_memory_size=*/256,
                                              /*trace=*/false);
  ASSERT_FALSE(threaded.has_value());
  EXPECT_EQ(threaded.error(), ProgramError::kRunGasTooLarge);
}

TEST(Rv32Threaded, containsPcBoundsAtInstructionCountNotByteCount)
{
  // Regression: code_ holds a byte span, so contains_pc must bound pc by the
  // instruction count, not code_.size() (the byte count). A two-instruction
  // program has valid pcs {0, 4} only; pc=8 is the first fall-off and every
  // 4-aligned pc beyond it must be rejected -- a byte-count bound would wrongly
  // accept pcs up to ~4x the program, letting out-of-range jal/branch targets
  // resolve to slots past the exec_ sentinel.
  const std::vector<uint32_t> words = {0x00000013U, 0x00000013U}; // two nops
  // Bind the byte image to a local: ThreadedProgram borrows it as a span, so it
  // must outlive `prog`.
  const std::vector<uint8_t> bytes = words_to_bytes(words);
  const auto prog = ThreadedProgram::create(bytes);
  ASSERT_TRUE(prog.has_value());
  EXPECT_TRUE(prog->contains_pc(0));
  EXPECT_TRUE(prog->contains_pc(4));
  EXPECT_FALSE(prog->contains_pc(8)); // first pc past the last instruction
  EXPECT_FALSE(prog->contains_pc(12));
  EXPECT_FALSE(prog->contains_pc(28)); // < byte-count*4, the buggy upper bound
  EXPECT_FALSE(prog->contains_pc(6));  // misaligned
}

// Smoke test: the threaded target builds and links. Real tests arrive in later
// tasks. Verifies HOGD_MUSTTAIL is defined (compiles) and a tail-call lowers.
[[nodiscard]] int
tail_leaf(int x)
{
  return x + 1;
}
[[nodiscard]] int
tail_caller(int x)
{
  HOGD_MUSTTAIL return tail_leaf(x);
}

TEST(Rv32Threaded, harnessBuildsAndMusttailLowers)
{
  EXPECT_EQ(tail_caller(41), 42);
}

using test_program::addi;
using test_program::make_program;
using test_program::op;

std::shared_ptr<const ThreadedMachinePreInstantiation>
threaded_from_words(std::span<const uint32_t> words,
                    uint32_t total_memory_size = 256, bool trace = false)
{
  auto pre_instantiation =
      ThreadedMachinePreInstantiation::create(words_to_bytes(words),
                                              /*memory=*/{}, /*memory_start=*/0,
                                              total_memory_size, trace);
  EXPECT_TRUE(pre_instantiation.has_value());
  return std::make_shared<const ThreadedMachinePreInstantiation>(
      std::move(pre_instantiation.value()));
}

ExecTrace
strip_trace_gas(ExecTrace trace)
{
  for (TraceStep &step : trace.steps) {
    step.state.gas_remaining = 0;
    step.state_before.gas_remaining = 0;
  }
  return trace;
}

void
expect_engine_only_fusion_coverage(
    const ThreadedMachinePreInstantiation &program, uint64_t seed)
{
  const FusionCoverage coverage =
      ThreadedMachinePreInstantiationTestPeer::fusion_coverage(program);
  EXPECT_GE(coverage.lui_addi, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.auipc_jalr, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.load_rr_rs1, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.load_rr_rs2, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.load_ri, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_alu_addi_addi, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_alu_addi_sll, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_alu_srl_addi, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_alu_sll_or, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_store, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_load, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.store_alu, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.load_store, 1U) << "engine seed " << seed;
  EXPECT_GE(coverage.alu_branch, 1U) << "engine seed " << seed;
}

TEST(Rv32Threaded, runsBasicArithmetic)
{
  // x1=5; x2=x1+7; x3=x1+x2. No terminator: falls off end -> bad-pc sentinel.
  const std::vector<uint32_t> words = {addi(1, 0, 5), addi(2, 1, 7),
                                       op(3, 1, 2, 0, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/100);

  RecordingHost host;
  const StepResult result = machine.run(host);

  EXPECT_EQ(result, StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(1), 5U);
  EXPECT_EQ(machine.reg(2), 12U);
  EXPECT_EQ(machine.reg(3), 17U);
  EXPECT_EQ(machine.reg(0), 0U);
}

TEST(Rv32Threaded, memorySlotRecycledAndReinitializedAcrossMachines)
{
  const std::vector<uint32_t> words = {addi(1, 0, 5), addi(2, 1, 7),
                                       op(3, 1, 2, 0, 0)};
  // Different memory sizes on purpose: the system allocator can hand the next
  // same-size allocation the just-freed block (which would fake a recycling
  // signal), but it cannot serve a kMaxMemoryBytes request from a freed
  // 256-byte block. Pool slots are reserved to kMaxMemoryBytes, so only real
  // pool recycling reproduces the first machine's buffer address here.
  auto small_program = threaded_from_words(words, /*total_memory_size=*/256);
  auto full_program =
      threaded_from_words(words, /*total_memory_size=*/kMaxMemoryBytes);

  const uint8_t *first_buffer = nullptr;
  {
    ThreadedMachine machine(*small_program, /*gas=*/100);
    first_buffer = machine.memory().data();
    // Dirty the buffer so recycling without re-initialization is detectable.
    std::ranges::fill(machine.memory(), uint8_t{0xab});
  } // destructor releases the buffer to the thread-local slot pool

  // LIFO pool on a single thread: the next machine must reuse the buffer the
  // first machine just released...
  ThreadedMachine machine(*full_program, /*gas=*/100);
  EXPECT_EQ(machine.memory().data(), first_buffer);
  // ...fully re-initialized from the (all-zero) image, no residue.
  for (size_t i = 0; i < machine.memory().size(); ++i) {
    ASSERT_EQ(machine.memory()[i], 0U) << "residue at byte " << i;
  }
  // And execution on the recycled slot behaves like a fresh machine.
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(3), 17U);
}

TEST(Rv32Threaded, liveMachinesHoldDistinctMemorySlots)
{
  const std::vector<uint32_t> words = {addi(1, 0, 5)};
  auto program = threaded_from_words(words);
  // Nested TryInvoke keeps multiple machines live on one thread; each must
  // hold its own buffer.
  ThreadedMachine a(*program, /*gas=*/100);
  ThreadedMachine b(*program, /*gas=*/100);
  EXPECT_NE(a.memory().data(), b.memory().data());
  std::ranges::fill(a.memory(), uint8_t{0xaa});
  std::ranges::fill(b.memory(), uint8_t{0xbb});
  EXPECT_EQ(a.memory()[0], 0xaaU);
  EXPECT_EQ(b.memory()[0], 0xbbU);
}

TEST(Rv32Threaded, movedFromMachineDoesNotReleaseItsSlot)
{
  const std::vector<uint32_t> words = {addi(1, 0, 5), addi(2, 1, 7),
                                       op(3, 1, 2, 0, 0)};
  auto program = threaded_from_words(words);

  std::optional<ThreadedMachine> source(ThreadedMachine(*program, /*gas=*/100));
  const uint8_t *buffer = source->memory().data();
  ThreadedMachine target(std::move(*source));
  // Destroying the moved-from machine must not return the buffer now owned
  // by the move target.
  source.reset();
  EXPECT_EQ(target.memory().data(), buffer);

  // If the moved-from destructor had wrongly released it, this machine would
  // acquire a buffer aliasing target's live memory.
  ThreadedMachine fresh(*program, /*gas=*/100);
  EXPECT_NE(fresh.memory().data(), target.memory().data());

  RecordingHost host;
  EXPECT_EQ(target.run(host), StepResult::kTrap);
  EXPECT_EQ(target.reg(3), 17U);
}

// Regression for issue #2824: moving a MemorySlot must transfer slot ownership
// and release the buffer exactly once. Ownership is "buf_ holds an allocation"
// (capacity != 0); the move resets the source to no allocation, so the transfer
// never depends on the implicit moved-from std::vector state -- the standard
// only guarantees a moved-from library object is "valid but unspecified". An
// acquired slot owns its reserved buffer even before it is filled, so no resize
// is needed to model ownership here.
TEST(Rv32Threaded, memorySlotMoveTransfersOwnershipExactlyOnce)
{
  memory_slot_pool() = MemorySlotPool{};
  {
    std::optional<MemorySlot> source(std::in_place);
    ASSERT_NE(source->bytes().capacity(), 0U); // acquired => owns its buffer
    std::optional<MemorySlot> target(std::in_place, std::move(*source));

    // The move handed the buffer to target and reset the source to no
    // allocation, independent of the std::vector's moved-from representation.
    EXPECT_NE(target->bytes().capacity(), 0U);
    EXPECT_EQ(source->bytes().capacity(), 0U);

    // Destroying the moved-from (allocation-less) source releases nothing.
    ASSERT_EQ(memory_slot_pool().count, 0U);
    source.reset();
    EXPECT_EQ(memory_slot_pool().count, 0U);

    // target is the sole owner; destroying it returns exactly one slot.
    target.reset();
    EXPECT_EQ(memory_slot_pool().count, 1U);
  }
  memory_slot_pool() = MemorySlotPool{};
}

// Moving an already-moved-from MemorySlot must stay safe: the first move resets
// the source to no allocation, so the second move sees a source with capacity 0
// and produces a target with capacity 0. Only the sole owner's slot is ever
// released -- a second move must not resurrect ownership and double-release.
TEST(Rv32Threaded, memorySlotDoubleMoveReleasesExactlyOnce)
{
  memory_slot_pool() = MemorySlotPool{};
  {
    std::optional<MemorySlot> a(std::in_place);
    std::optional<MemorySlot> b(std::in_place, std::move(*a)); // b owns it
    std::optional<MemorySlot> c(std::in_place,
                                std::move(*a)); // a already reset

    EXPECT_NE(b->bytes().capacity(), 0U);
    EXPECT_EQ(a->bytes().capacity(), 0U);
    EXPECT_EQ(c->bytes().capacity(), 0U);

    // Neither the moved-from a nor c (moved from a reset source) owns a slot.
    a.reset();
    c.reset();
    EXPECT_EQ(memory_slot_pool().count, 0U);

    // b is the sole owner; destroying it returns exactly one slot.
    b.reset();
    EXPECT_EQ(memory_slot_pool().count, 1U);
  }
  memory_slot_pool() = MemorySlotPool{};
}

TEST(Rv32Threaded, branchTakenSkipsInstruction)
{
  using test_program::branch;
  // x1=1, x2=1; beq x1,x2,+8 (taken) skips an addi that would set x3=99.
  const std::vector<uint32_t> words = {addi(1, 0, 1), addi(2, 0, 1),
                                       branch(0, 1, 2, 8), addi(3, 0, 99)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(3), 0U);                   // branch skipped the addi
}

TEST(Rv32Threaded, branchNotTakenFallsThrough)
{
  using test_program::branch;
  // x1=1, x2=2; beq x1,x2,+8 (NOT taken) -> next addi sets x3=99 executes.
  const std::vector<uint32_t> words = {addi(1, 0, 1), addi(2, 0, 2),
                                       branch(0, 1, 2, 8), addi(3, 0, 99)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(3), 99U);
}

TEST(Rv32Threaded, jalLinksAndJumps)
{
  using test_program::j_type;
  // jal x1,+8 writes link (pc+4=4) to x1 and jumps over the addi at pc=4.
  // Use x4 for the skipped instruction (x2 was avoided when sp seeded
  // nonzero at entry; sp now seeds 0 but x4 keeps the test stable).
  const std::vector<uint32_t> words = {j_type(1, 8), addi(4, 0, 7),
                                       addi(3, 0, 9)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(1), 4U);
  EXPECT_EQ(machine.reg(4), 0U); // skipped
  EXPECT_EQ(machine.reg(3), 9U); // target executed
}

TEST(Rv32Threaded, jalBadTargetTrapsAfterGas)
{
  using test_program::j_type;
  const std::vector<uint32_t> words = {j_type(0, 4096)}; // target out of range
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.gas_remaining(), 99U); // jal gas charged before the trap
}

TEST(Rv32Threaded, branchBadTargetTakenTrapsBadPc)
{
  using test_program::branch;
  // beq x0,x0,+4096: target is out of range, so op_branch_bad_target (baked
  // with beq_cmp) handles it; the comparison is equal, so the taken path traps
  // kBadPc rather than falling through.
  const std::vector<uint32_t> words = {branch(0, 0, 0, 4096)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
}

TEST(Rv32Threaded, branchBadTargetNotTakenFallsThrough)
{
  using test_program::branch;
  // x1=1, x2=2; beq x1,x2,+4096 has an out-of-range target (bad_target handler,
  // beq_cmp), but 1 != 2 so the branch is not taken: it falls through to the
  // addi, which runs (x3=99). Exercises the not-taken path's baked Cmp.
  const std::vector<uint32_t> words = {addi(1, 0, 1), addi(2, 0, 2),
                                       branch(0, 1, 2, 4096), addi(3, 0, 99)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end after addi
  EXPECT_EQ(machine.reg(3), 99U);                  // fall-through executed
}

TEST(Rv32Threaded, outOfGasMidRunTrapsAtRunHead)
{
  using test_program::branch;
  // Run head at pc=0: [addi x5 (body), beq x0,x0,+8 (barrier)] -> target pc=12.
  // Every instruction costs 1 gas, so the run (body + barrier) costs 2. With a
  // budget of 1 the body alone would fit, but the barrier's gas is folded into
  // the run-head charge, so enter_run aborts the whole run at its head: OOG at
  // pc=0 and the body never executes.
  const std::vector<uint32_t> words = {addi(5, 0, 1), branch(0, 0, 0, 8),
                                       addi(6, 0, 99), addi(7, 0, 42)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/1);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_EQ(machine.pc(), 0U); // aborted at the run head, not the beq
  EXPECT_EQ(machine.gas_remaining(),
            0U); // unaffordable run is fully debited (saturates to 0), matching
                 // per-instruction draining
  EXPECT_EQ(machine.reg(5), 0U); // body did not execute
}

TEST(Rv32Threaded, fusedRunGasMatchesPerInstructionAccountingOnOutOfGas)
{
  using test_program::branch;
  // The block-granular charger bills a whole run at its head in one check. For
  // a run of 1-gas instructions, that must be indistinguishable from debiting
  // one gas per instruction: under every budget, the gas left at termination --
  // including the out-of-gas case -- must agree with per-instruction metering.
  //
  // Two programs do the same kWork units of 1-gas work, then fall off the end:
  //   fused:     kWork back-to-back addi nops -> ONE run of run_gas == kWork.
  //   per_instr: (kWork-1) always-taken beq(+4), each landing on the next, then
  //              a trailing addi nop -> kWork single-instruction runs (run_gas
  //              1 each). beq and addi both cost kBaseInstructionGas (1), so
  //              this is literal one-gas-at-a-time accounting -- the ground
  //              truth.
  // Block metering must match it for all budgets, so on OOG the fused run head
  // debits the whole (unaffordable) run -- saturating gas_remaining to 0 --
  // exactly as the per-instruction program drains to 0 before it traps.
  constexpr uint32_t kWork = 6;

  std::vector<uint32_t> fused_words;
  fused_words.reserve(kWork);
  for (uint32_t i = 0; i < kWork; ++i) {
    fused_words.push_back(addi(0, 0, 0)); // nop: x0 = x0 + 0
  }
  auto fused_program = threaded_from_words(fused_words);

  std::vector<uint32_t> per_instr_words;
  per_instr_words.reserve(kWork);
  for (uint32_t i = 0; i + 1 < kWork; ++i) {
    per_instr_words.push_back(branch(0, 0, 0, 4)); // beq x0,x0,+4 -> next instr
  }
  per_instr_words.push_back(
      addi(0, 0, 0)); // trailing nop the last branch lands on
  auto per_instr_program = threaded_from_words(per_instr_words);

  // Sweep every budget from starved through comfortably past the work.
  for (uint64_t gas = 0; gas <= kWork + 2; ++gas) {
    ThreadedMachine fused_machine(*fused_program, gas);
    ThreadedMachine per_instr_machine(*per_instr_program, gas);
    RecordingHost host;
    const StepResult fused_result = fused_machine.run(host);
    const StepResult per_instr_result = per_instr_machine.run(host);

    EXPECT_EQ(fused_machine.gas_remaining(), per_instr_machine.gas_remaining())
        << "gas budget " << gas;
    EXPECT_EQ(fused_result, per_instr_result) << "gas budget " << gas;
    EXPECT_EQ(fused_machine.trap(), per_instr_machine.trap())
        << "gas budget " << gas;
  }
}

TEST(Rv32Threaded, tightSelfLoopRunsOutOfGas)
{
  using test_program::branch;
  // beq x0,x0,0 branches to itself: an empty-body loop whose only gas is the
  // branch's own. The branch is its own run head, so enter_run charges its gas
  // every iteration and the loop terminates with kOutOfGas (rather than
  // spinning forever once the per-barrier self-charge is gone).
  const std::vector<uint32_t> words = {branch(0, 0, 0, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/8);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_EQ(machine.gas_remaining(),
            0U); // all 8 gas consumed, one per iteration
}

// jalr and auipc are corpus blind spots: the generators exclude jalr (computed
// target) and auipc/linking-jal (pc-relative register writes the Spike oracle
// can't model), so the capstone diff never reaches them. Cover them directly.
TEST(Rv32Threaded, jalrLinksAndJumpsToComputedTarget)
{
  using test_program::jalr;
  // x1=12; jalr x5,x1,0 -> link=pc+4=8 to x5, target=(12)&~1=12 (skips pc=8).
  const std::vector<uint32_t> words = {addi(1, 0, 12), jalr(5, 1, 0),
                                       addi(3, 0, 99), addi(4, 0, 9)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host),
            StepResult::kTrap);  // falls off after target
  EXPECT_EQ(machine.reg(5), 8U); // link = pc_of_jalr (4) + 4
  EXPECT_EQ(machine.reg(3), 0U); // pc=8 skipped
  EXPECT_EQ(machine.reg(4), 9U); // target pc=12 executed
}

TEST(Rv32Threaded, auipcIsPcRelative)
{
  using test_program::u_type;
  // nop at pc=0; auipc x1, 0 at pc=4 -> x1 = pc(4) + (0 << 12) = 4.
  const std::vector<uint32_t> words = {addi(0, 0, 0), u_type(0x17U, 1, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(1), 4U);
}

TEST(Rv32Threaded, storeThenLoadRoundTrips)
{
  using test_program::load;
  using test_program::store;
  // x1=0 (base); x3=0x7ff (data); sw x3,16(x1); lw x4,16(x1). x4 should read
  // back 0x7ff. (x2 avoided: it is the initialized stack pointer.)
  const std::vector<uint32_t> words = {addi(1, 0, 0), addi(3, 0, 0x7ff),
                                       store(1, 3, 16, 2), load(4, 2, 1, 16)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(4), 0x7ffU);
}

TEST(Rv32Threaded, signedByteLoadSignExtends)
{
  using test_program::load;
  using test_program::store;
  // Store byte 0x80 at addr 8; lb sign-extends to 0xffffff80; lbu zero-extends
  // to 0x80. This is the one path the SignExtend template parameter governs.
  const std::vector<uint32_t> words = {addi(1, 0, 0), addi(3, 0, 0x80),
                                       store(1, 3, 8, 0), // sb
                                       load(4, 0, 1, 8),  // lb
                                       load(5, 4, 1, 8)}; // lbu
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(4), 0xffffff80U); // lb: sign-extended
  EXPECT_EQ(machine.reg(5), 0x80U);       // lbu: zero-extended
}

TEST(Rv32Threaded, unalignedLoadReassemblesLittleEndian)
{
  using test_program::load;
  using test_program::store;
  // sw x3 (0x100) at 16 and sw x5 (0x77) at 20, then lw at 17: bytes 17..20
  // are 01 00 00 77 -> x4 = 0x77000001. The nonzero byte at 20 pins the
  // cross-boundary read (a zero there could not tell "read" from "zeroed").
  const std::vector<uint32_t> words = {
      addi(1, 0, 16),    // x1 = 16
      addi(3, 0, 0x100), // x3 = 0x100
      store(1, 3, 0, 2), // sw x3, 0(x1)
      addi(5, 0, 0x77),  // x5 = 0x77
      store(1, 5, 4, 2), // sw x5, 4(x1) -> nonzero sentinel at 20
      addi(2, 1, 1),     // x2 = 17
      load(4, 2, 2, 0),  // lw x4, 0(x2) -> unaligned address 17
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(4), 0x77000001U);
}

TEST(Rv32Threaded, unalignedStoreWritesExactBytes)
{
  using test_program::load;
  using test_program::store;
  using test_program::u_type;
  // Pre-fill words at 0 and 4 with 0x11111111, then sw x3 (0x1ff) at address
  // 1: bytes 0..7 become 11 ff 01 00 00 11 11 11. The nonzero fill proves the
  // store wrote exactly bytes 1..4 (byte 4's 0x00 came from the store, not
  // from untouched memory).
  const std::vector<uint32_t> words = {
      addi(6, 0, 0x111),             // x6 = 0x111
      u_type(0x37U, 7, 0x11111000U), // lui x7, 0x11111
      op(6, 7, 6, 0, 0),             // add x6, x7, x6 -> x6 = 0x11111111
      store(0, 6, 0, 2),             // sw x6, 0(x0)
      store(0, 6, 4, 2),             // sw x6, 4(x0)
      addi(3, 0, 0x1ff),             // x3 = 0x1ff
      store(0, 3, 1, 2),             // sw x3, 1(x0) -> unaligned address 1
      load(4, 2, 0, 0),              // lw x4, 0(x0) -> bytes 0..3 = 11 ff 01 00
      load(5, 2, 0, 4),              // lw x5, 4(x0) -> bytes 4..7 = 00 11 11 11
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(4), 0x0001ff11U);
  EXPECT_EQ(machine.reg(5), 0x11111100U);
}

TEST(Rv32Threaded, outOfBoundsStoreTraps)
{
  using test_program::store;
  // x1=0; sw x3, 2044(x1) -> address 2044 >= memory size 256 -> kBadMemory.
  // 2044 + 4 overruns the 256-byte memory, so the bounds check fires.
  const std::vector<uint32_t> words = {addi(1, 0, 0), store(1, 3, 2044, 2)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
}

TEST(Rv32Threaded, endOverlapLoadTraps)
{
  using test_program::load;
  // x1=254; lw x4,0(x1): 254 is in [0,256) but 254+4 overruns the 256-byte
  // memory -> kBadMemory (unaligned end-overlap; alignment itself is legal).
  const std::vector<uint32_t> words = {addi(1, 0, 254), load(4, 2, 1, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  EXPECT_EQ(machine.reg(4), 0U); // load never wrote
}

// --- load + reg-arith fusion (EXPERIMENT(fuse)) -----------------------------
// A lw immediately followed by a fusible R-type runs through op_load_rr (one
// dispatch for the pair). The corpus differential test is the broad net; these
// pin the three behaviors fusion must preserve: fall-through result, a branch
// landing on the fused tail, and a faulting load suppressing the rr.

TEST(Rv32Threaded, fusedLoadFeedsRegArith)
{
  using test_program::load;
  using test_program::op;
  using test_program::store;
  // Store 0x100 at addr 16; lw x4,16(x1); add x5,x4,x6 (x6=7) -> x5=0x107. The
  // lw+add is the fused pair and the load result feeds the add.
  const std::vector<uint32_t> words = {
      addi(1, 0, 0),      // x1 = 0 (base)
      addi(3, 0, 0x100),  // x3 = 0x100 (data)
      addi(6, 0, 7),      // x6 = 7
      store(1, 3, 16, 2), // sw x3, 16(x1)
      load(4, 2, 1, 16),  // lw x4, 16(x1)   <- fused head
      op(5, 4, 6, 0, 0),  // add x5, x4, x6  <- fused tail
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(4), 0x100U);
  EXPECT_EQ(machine.reg(5), 0x107U);
}

TEST(Rv32Threaded, fusedLoadFeedsRs1OfNonCommutativeOp)
{
  using test_program::load;
  using test_program::op;
  using test_program::store;
  // Store 0x100 @16; lw x4,16(x1); sub x5,x4,x6 (x6=7) -> x5 = 0x100-7 = 0xf9.
  // The loaded value is rs1 of a non-commutative op, so operand order matters:
  // a wrong substitution (v as rs2) would give 7-0x100. funct7=0x20 -> sub.
  const std::vector<uint32_t> words = {
      addi(1, 0, 0),        // x1 = 0 (base)
      addi(3, 0, 0x100),    // x3 = 0x100 (data)
      addi(6, 0, 7),        // x6 = 7
      store(1, 3, 16, 2),   // sw x3, 16(x1)
      load(4, 2, 1, 16),    // lw x4, 16(x1)     <- fused head
      op(5, 4, 6, 0, 0x20), // sub x5, x4, x6 (x4 is rs1)  <- fused tail
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(5), 0xf9U);
}

TEST(Rv32Threaded, fusedLoadFeedsRs2OfNonCommutativeOp)
{
  using test_program::load;
  using test_program::op;
  using test_program::store;
  // Store 0x100 @16; lw x4,16(x1); sub x5,x6,x4 (x6=0x200) -> x5 = 0x200-0x100.
  // The loaded value is rs2 here (fib's `add a0,a0,t1` shape, non-commutative
  // to catch operand-order bugs). funct7=0x20 -> sub.
  const std::vector<uint32_t> words = {
      addi(1, 0, 0),        // x1 = 0 (base)
      addi(3, 0, 0x100),    // x3 = 0x100 (data)
      addi(6, 0, 0x200),    // x6 = 0x200
      store(1, 3, 16, 2),   // sw x3, 16(x1)
      load(4, 2, 1, 16),    // lw x4, 16(x1)     <- fused head
      op(5, 6, 4, 0, 0x20), // sub x5, x6, x4 (x4 is rs2)  <- fused tail
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(5), 0x100U);
}

TEST(Rv32Threaded, branchOntoFusedRrSlotExecutesStandalone)
{
  using test_program::branch;
  using test_program::load;
  using test_program::op;
  // beq x0,x0,+8 jumps over the lw and lands directly on the add (the fused
  // pair's tail). The pair is a load->use (add reads x4, the load's dest) so it
  // fuses; the add must still run standalone via the slot's own handler. x4
  // stays 0 (lw skipped); add x5,x4,x6 (x4=0, x6=5) -> x5=5.
  const std::vector<uint32_t> words = {
      addi(6, 0, 5),      // x6 = 5
      branch(0, 0, 0, 8), // beq x0,x0,+8 -> skip lw, land on add
      load(4, 2, 6, 0),   // lw x4, 0(x6)   (skipped)         <- fused head
      op(5, 4, 6, 0, 0),  // add x5, x4, x6 (branch target)   <- fused tail
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end after add
  EXPECT_EQ(machine.reg(4), 0U);                   // lw was skipped
  EXPECT_EQ(machine.reg(5), 5U); // add ran standalone (x4=0 + x6=5)
}

TEST(Rv32Threaded, fusedLoadFaultSuppressesRegArith)
{
  using test_program::load;
  using test_program::op;
  // Out-of-bounds lw (addr 1024 >= memory size 256) fused with a load->use add
  // x5,x4,x6 (reads x4, the load's dest). The load faults (kBadMemory) and the
  // add must NOT execute -> x5 stays 0.
  const std::vector<uint32_t> words = {
      addi(1, 0, 1024),  // x1 = 1024 (out of bounds base)
      addi(6, 0, 5),     // x6 = 5
      load(4, 2, 1, 0),  // lw x4, 0(x1) -> addr 1024, OOB  <- fused head
      op(5, 4, 6, 0, 0), // add x5, x4, x6 (uses x4)        <- fused tail
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  EXPECT_EQ(machine.reg(4), 0U); // load never wrote
  EXPECT_EQ(machine.reg(5), 0U); // add never ran (fault is terminal)
}

TEST(Rv32Threaded, fusedLoadFeedsImmArith)
{
  using test_program::load;
  using test_program::store;
  // Store 0x100 at addr 16; lw x4,16(x1); addi x4,x4,-2 -> x4=0xfe. The lw+addi
  // is the I-type fused pair (op_load_ri); the load result feeds the addi. This
  // mirrors fib's `lw a0,8(sp); addi a0,a0,-2` hot pair.
  const std::vector<uint32_t> words = {
      addi(1, 0, 0),      // x1 = 0 (base)
      addi(3, 0, 0x100),  // x3 = 0x100 (data)
      store(1, 3, 16, 2), // sw x3, 16(x1)
      load(4, 2, 1, 16),  // lw x4, 16(x1)    <- fused head
      addi(4, 4, -2),     // addi x4, x4, -2  <- fused tail (I-type)
  };
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end
  EXPECT_EQ(machine.reg(4), 0xfeU);
}

TEST(Rv32Threaded, ebreakTraps)
{
  // ebreak followed by an addi. ebreak is a barrier, so its run's gas_to_run
  // is only its own gas (kBaseInstructionGas=1); the addi's gas must NOT be
  // pre-charged. Starting gas=100 -> gas_remaining=99 after the run fires.
  // If ebreak were not a barrier, gas_to_run would be 2 (ebreak+addi) and
  // gas_remaining would be 98 -- detecting removal of is_barrier(Op::kEbreak).
  const std::vector<uint32_t> words = {0x00100073U,
                                       addi(1, 0, 1)}; // ebreak, addi x1,x0,1
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBreakpoint);
  EXPECT_EQ(machine.gas_remaining(), 99U); // only ebreak's own gas charged
}

TEST(Rv32Threaded, unimpTraps)
{
  // unimp followed by an addi. unimp is a barrier, so its run's gas_to_run
  // is only its own gas (kBaseInstructionGas=1); the addi's gas must NOT be
  // pre-charged. Starting gas=100 -> gas_remaining=99 after the run fires.
  // If unimp were not a barrier, gas_to_run would be 2 (unimp+addi) and
  // gas_remaining would be 98 -- detecting removal of is_barrier(Op::kUnimp).
  const std::vector<uint32_t> words = {0xc0001073U,
                                       addi(1, 0, 1)}; // unimp, addi x1,x0,1
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kInvalidInstruction);
  EXPECT_EQ(machine.gas_remaining(), 99U); // only unimp's own gas charged
}

TEST(Rv32Threaded, unimpDecodesToKUnimp)
{
  // 0xc0001073 is the canonical RISC-V `unimp` word (csrrw x0, cycle, x0).
  // Pin that ThreadedProgram accepts it and decodes it to Op::kUnimp.
  const std::vector<uint32_t> words = {0xc0001073U};
  const std::vector<uint8_t> bytes = words_to_bytes(words);
  const auto prog = ThreadedProgram::create(bytes);
  ASSERT_TRUE(prog.has_value());
  const auto decoded = prog->instruction_at(0);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->op, Op::kUnimp);
}

TEST(Rv32Threaded, unimpTrapsInvalidInstruction)
{
  const std::vector<uint32_t> words = {0xc0001073U}; // unimp
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kInvalidInstruction);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.gas_remaining(), 99U);

  ThreadedMachine out_of_gas(*program, 0);
  EXPECT_EQ(out_of_gas.run(host), StepResult::kTrap);
  EXPECT_EQ(out_of_gas.trap(), Trap::kOutOfGas);
  EXPECT_EQ(out_of_gas.pc(), 0U);
  EXPECT_EQ(out_of_gas.gas_remaining(), 0U);
}

TEST(Rv32Threaded, ecallHaltsWithA0)
{
  // x10(a0)=7, then ecall. RecordingHost default halts with a0.
  const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kHalted);
  EXPECT_EQ(machine.halt_code(), 7U);
  ASSERT_EQ(host.call_count(), 1U);
  EXPECT_EQ(host.calls()[0].a0, 7U);
}

TEST(Rv32Threaded, ecallContinueWritesA0AndAdvances)
{
  const std::vector<uint32_t> words = {addi(10, 0, 1), 0x00000073U,
                                       addi(11, 0, 5)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  RecordingHost host(RecordingHost::OnEcall::kContinue);
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off after addi
  EXPECT_EQ(machine.reg(10), 0U); // continue_with(0) wrote a0=0
  EXPECT_EQ(machine.reg(11), 5U); // execution continued past ecall
}

// Far above any corpus program's need (body_len 96 -> ~97 instructions), so
// runs are never gas-limited and the diff reflects pure ISA semantics.
constexpr uint64_t kCorpusGasBudget = 1'000'000;

TEST(Rv32Threaded, matchesCanonicalMachineOverCorpus)
{
  auto check = [](const std::vector<uint32_t> &words, uint64_t seed,
                  const char *kind, uint32_t memory_bytes) {
    const std::vector<uint8_t> bytes = words_to_bytes(words);

    // Canonical reference trace.
    const auto canonical =
        make_program(std::span<const uint32_t>(words.data(), words.size()));
    ASSERT_NE(canonical, nullptr) << kind << " seed " << seed;
    Machine cmachine(canonical, memory_bytes, kCorpusGasBudget);
    RecordingHost chost;
    const ExecTrace ctrace = trace_execution(cmachine, chost);

    // Threaded trace (same program, same memory size and gas -> a fair diff).
    auto threaded = threaded_from_words(words, memory_bytes, /*trace=*/true);
    ThreadedMachine tmachine(*threaded, kCorpusGasBudget);
    RecordingHost thost;
    TraceObserver tobs;
    tmachine.run(thost, tobs);

    // The threaded machine meters gas at start-of-run (block granularity), so
    // its intermediate gas_remaining runs ahead of the canonical machine's
    // per-instruction trajectory. Everything else -- pc, regs, memory, control
    // flow, per-step result -- is byte-identical, and the total gas consumed
    // matches (gas charged up to any barrier is the same under either scheme).
    // So normalize intermediate gas_remaining out of the per-step comparison
    // and check the final gas_remaining separately.
    const ExecTrace cstripped = strip_trace_gas(ctrace);
    const ExecTrace tstripped = strip_trace_gas(tobs.trace());

    EXPECT_EQ(describe_trace_divergence(cstripped, tstripped), "")
        << kind << " seed " << seed;
    // describe_trace_divergence compares step.state (post-step) and step.result
    // but NOT step.state_before, so check that field separately -- it's the
    // guard against observer state-chaining bugs.
    ASSERT_EQ(tstripped.steps.size(), cstripped.steps.size())
        << kind << " seed " << seed;
    for (size_t i = 0; i < cstripped.steps.size(); ++i) {
      EXPECT_EQ(tstripped.steps[i].state_before,
                cstripped.steps[i].state_before)
          << kind << " seed " << seed << " step " << i;
    }
    // Total gas consumed must match: gas at every barrier (incl. the terminal
    // ecall) is identical under block vs per-instruction metering.
    ASSERT_FALSE(ctrace.steps.empty());
    EXPECT_EQ(tobs.trace().steps.back().state.gas_remaining,
              ctrace.steps.back().state.gas_remaining)
        << kind << " seed " << seed;
  };

  // 64 seeds (more than the decode test's 16: execution is the primary
  // differential signal). Register-only programs never touch memory, so 4096 is
  // arbitrary-but-sufficient; memory programs need the corpus data region.
  for (uint64_t seed = 0; seed < 64; ++seed) {
    check(test_corpus::generate_register_only_program(seed, 96), seed, "reg",
          4096);
    check(test_corpus::generate_memory_program(seed, 96), seed, "mem",
          test_corpus::kCorpusMemoryBytes);
  }
}

TEST(Rv32Threaded, matchesCanonicalMachineOverEngineOnlyCorpus)
{
  constexpr uint32_t kMemoryBytes = 4096;
  auto check_exact = [](const std::vector<uint32_t> &words, uint64_t seed) {
    const auto canonical =
        make_program(std::span<const uint32_t>(words.data(), words.size()));
    ASSERT_NE(canonical, nullptr) << "engine seed " << seed;
    Machine cmachine(canonical, kMemoryBytes, kCorpusGasBudget);
    RecordingHost chost(RecordingHost::OnEcall::kContinue);
    const ExecTrace ctrace = trace_execution(cmachine, chost);

    auto threaded = threaded_from_words(words, kMemoryBytes, /*trace=*/true);
    expect_engine_only_fusion_coverage(*threaded, seed);
    ThreadedMachine tmachine(*threaded, kCorpusGasBudget);
    RecordingHost thost(RecordingHost::OnEcall::kContinue);
    TraceObserver tobs;
    tmachine.run(thost, tobs);

    const ExecTrace cstripped = strip_trace_gas(ctrace);
    const ExecTrace tstripped = strip_trace_gas(tobs.trace());

    EXPECT_EQ(describe_trace_divergence(cstripped, tstripped), "")
        << "engine seed " << seed;
    ASSERT_EQ(tstripped.steps.size(), cstripped.steps.size())
        << "engine seed " << seed;
    for (size_t i = 0; i < cstripped.steps.size(); ++i) {
      EXPECT_EQ(tstripped.steps[i].state_before,
                cstripped.steps[i].state_before)
          << "engine seed " << seed << " step " << i;
    }
    EXPECT_EQ(tobs.trace().steps.back().state.gas_remaining,
              ctrace.steps.back().state.gas_remaining)
        << "engine seed " << seed;
    EXPECT_EQ(thost.call_count(), chost.call_count()) << "engine seed " << seed;
  };

  for (uint64_t seed = 0; seed < 64; ++seed) {
    check_exact(test_corpus::generate_engine_only_program(seed), seed);
  }
}

TEST(Rv32Threaded, engineOnlyCorpusGasBudgetSweep)
{
  constexpr uint32_t kMemoryBytes = 4096;
  auto add_budget = [](std::vector<uint64_t> &budgets, uint64_t budget) {
    if (std::ranges::find(budgets, budget) == budgets.end()) {
      budgets.push_back(budget);
    }
  };

  auto check_budget = [](const std::vector<uint32_t> &words,
                         const ThreadedMachinePreInstantiation &threaded,
                         uint64_t seed, uint64_t budget) {
    const auto canonical =
        make_program(std::span<const uint32_t>(words.data(), words.size()));
    ASSERT_NE(canonical, nullptr) << "engine seed " << seed;
    Machine cmachine(canonical, kMemoryBytes, budget);
    RecordingHost chost(RecordingHost::OnEcall::kContinue);
    const ExecTrace ctrace = trace_execution(cmachine, chost);

    ThreadedMachine tmachine(threaded, budget);
    RecordingHost thost(RecordingHost::OnEcall::kContinue);
    TraceObserver tobs;
    const StepResult tresult = tmachine.run(thost, tobs);

    const bool canonical_oog = cmachine.trap() == Trap::kOutOfGas;
    const bool threaded_oog = tmachine.trap() == Trap::kOutOfGas;
    EXPECT_EQ(threaded_oog, canonical_oog)
        << "engine seed " << seed << " gas budget " << budget;
    if (canonical_oog || threaded_oog) {
      if (threaded_oog) {
        EXPECT_EQ(tmachine.gas_remaining(), 0U)
            << "engine seed " << seed << " gas budget " << budget;
      }
      EXPECT_EQ(ctrace.final_result, StepResult::kTrap)
          << "engine seed " << seed << " gas budget " << budget;
      EXPECT_EQ(tresult, StepResult::kTrap)
          << "engine seed " << seed << " gas budget " << budget;
      return;
    }

    const ExecTrace cstripped = strip_trace_gas(ctrace);
    const ExecTrace tstripped = strip_trace_gas(tobs.trace());
    EXPECT_EQ(describe_trace_divergence(cstripped, tstripped), "")
        << "engine seed " << seed << " gas budget " << budget;
    EXPECT_EQ(tobs.trace().steps.back().state.gas_remaining,
              ctrace.steps.back().state.gas_remaining)
        << "engine seed " << seed << " gas budget " << budget;
    EXPECT_EQ(thost.call_count(), chost.call_count())
        << "engine seed " << seed << " gas budget " << budget;
  };

  for (uint64_t seed = 0; seed < 32; ++seed) {
    const std::vector<uint32_t> words =
        test_corpus::generate_engine_only_program(seed);
    auto threaded = threaded_from_words(words, kMemoryBytes, /*trace=*/true);
    expect_engine_only_fusion_coverage(*threaded, seed);

    const auto canonical =
        make_program(std::span<const uint32_t>(words.data(), words.size()));
    ASSERT_NE(canonical, nullptr) << "engine seed " << seed;
    Machine full_machine(canonical, kMemoryBytes, kCorpusGasBudget);
    RecordingHost full_host(RecordingHost::OnEcall::kContinue);
    const ExecTrace full_trace = trace_execution(full_machine, full_host);
    ASSERT_FALSE(full_trace.steps.empty()) << "engine seed " << seed;
    const uint64_t full_cost =
        kCorpusGasBudget - full_trace.steps.back().state.gas_remaining;

    std::vector<uint64_t> budgets;
    add_budget(budgets, 0);
    add_budget(budgets, 1);
    add_budget(budgets, full_cost / 2);
    if (full_cost > 0) {
      add_budget(budgets, full_cost - 1);
    }
    add_budget(budgets, full_cost);
    add_budget(budgets, full_cost + 1);

    std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);
    for (size_t i = 0; i < 8; ++i) {
      add_budget(budgets, rng() % (full_cost + 16));
    }

    for (uint64_t budget : budgets) {
      check_budget(words, *threaded, seed, budget);
    }
  }
}

TEST(Rv32Threaded, seedsMethodIdIntoA0A1)
{
  // Mirrors Rv32MachineSeed.seedsMethodIdIntoA0A1 for the canonical Machine.
  auto program = threaded_from_words(std::vector<uint32_t>{addi(0, 0, 0)});
  ThreadedMachine machine(*program, /*gas=*/100);
  machine.seed_method_id(0xCA571C66A719DD56ULL);
  EXPECT_EQ(machine.reg(10), 0xA719DD56U); // a0 = low 32 bits
  EXPECT_EQ(machine.reg(11), 0xCA571C66U); // a1 = high 32 bits
}

TEST(Rv32Threaded, seededMethodIdIsLiveAtExecutionStart)
{
  // The v4 guest dispatcher reads the method id from a0 at _start, so a seed
  // set before run() must be visible to executing instructions. x5 := a0.
  const std::vector<uint32_t> words = {addi(5, 10, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/100);
  machine.seed_method_id(0x00000000DEADBEEFULL);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap); // falls off end -> bad pc
  EXPECT_EQ(machine.reg(5), 0xDEADBEEFU);
}

TEST(Rv32Threaded, untracedRunCompletes)
{
  // The zero-cost production path (run(host), untraced handlers) builds and
  // runs to the ecall halt.
  const std::vector<uint32_t> words =
      test_corpus::generate_register_only_program(123, 64);
  auto program = threaded_from_words(words, /*total_memory_size=*/4096);
  ThreadedMachine machine(*program, kCorpusGasBudget);
  RecordingHost host;
  EXPECT_EQ(machine.run(host),
            StepResult::kHalted); // corpus ends in ecall
}

TEST(Rv32ThreadedYieldApi, startYieldsEcallThenResumeContinues)
{
  // a0=1 ; ecall ; a1=5 -> resume_from_ecall(continue_with(9)) writes a0, runs
  // on.
  const std::vector<uint32_t> words = {addi(10, 0, 1), 0x00000073U,
                                       addi(11, 0, 5)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);

  EXPECT_EQ(machine.start(), StepResult::kEcall);
  ASSERT_TRUE(machine.pending_ecall().has_value());
  EXPECT_EQ(machine.pending_ecall()->a0, 1U);

  const StepResult r = machine.resume_from_ecall(HostResult::continue_with(9));
  EXPECT_EQ(r, StepResult::kTrap); // falls off the end after addi a1
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(10), 9U);
  EXPECT_EQ(machine.reg(11), 5U);
}

TEST(Rv32ThreadedYieldApi, resumeFromEcallHaltIsTerminal)
{
  const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  EXPECT_EQ(machine.start(), StepResult::kEcall);
  EXPECT_EQ(machine.resume_from_ecall(HostResult::halt(7)),
            StepResult::kHalted);
  EXPECT_EQ(machine.halt_code(), 7U);
}

TEST(Rv32ThreadedYieldApi, resumeFromEcallRejectsHostTrap)
{
  // The threaded ecall seam does not support host traps: unlike baseline
  // Machine it has no host_trap_payload_ to carry the deterministic-vs-fatal
  // kind/code that runtime_context relies on, and its only production driver
  // (rv32_contract.cc) resolves host errors itself and never feeds a kTrap
  // HostResult back in. So a kTrap result is a contract violation and aborts
  // rather than silently collapsing to a payload-less kHostTrap.
  const std::vector<uint32_t> words = {0x00000073U};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, 100);
  EXPECT_EQ(machine.start(), StepResult::kEcall);
  EXPECT_DEATH(
      {
        [[maybe_unused]] const StepResult ignored =
            machine.resume_from_ecall(HostResult::trap());
      },
      "");
}

TEST(Rv32ThreadedYieldApi, startResumeMatchesRunForSameHost)
{
  // RecordingHost's default (kHaltWithA0) returns halt(view.reg(10)), so this
  // program (a0=7 ; ecall) halts with code 7 under both drivers.
  const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};

  auto p1 = threaded_from_words(words);
  ThreadedMachine driven(*p1, 100);
  RecordingHost manual_host;
  StepResult r = driven.start();
  // Mid-flight: assert the suspend state run() hides.
  EXPECT_EQ(r, StepResult::kEcall);
  ASSERT_TRUE(driven.pending_ecall().has_value());
  while (r == StepResult::kEcall) {
    r = driven.resume_from_ecall(
        manual_host.ecall(driven.view(), driven.pending_ecall().value()));
  }

  auto p2 = threaded_from_words(words);
  ThreadedMachine ran(*p2, 100);
  RecordingHost run_host;
  const StepResult rr = ran.run(run_host);

  EXPECT_EQ(r, rr);
  EXPECT_EQ(r, StepResult::kHalted); // default host halts with a0=7
  EXPECT_EQ(driven.halt_code(), ran.halt_code());
  EXPECT_EQ(driven.halt_code(), 7U);
  EXPECT_EQ(driven.pc(), ran.pc());
  EXPECT_EQ(driven.gas_remaining(), ran.gas_remaining());
}

TEST(Rv32ThreadedYieldApi, hostMemoryWritePersistsOnContinue)
{
  // a0=1 ; ecall ; a1=5. The host writes 4 bytes at offset 4 through view(),
  // then continues. The writes must be observable in machine.memory() after the
  // run completes (no per-ecall rollback).
  const std::vector<uint32_t> words = {addi(10, 0, 1), 0x00000073U,
                                       addi(11, 0, 5)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/100);

  ThreadedMemoryHost host(ThreadedMemoryHost::Outcome::kContinue);
  const StepResult result = machine.run(host);

  EXPECT_EQ(result, StepResult::kTrap); // falls off the end after addi a1
  EXPECT_TRUE(host.wrote);
  EXPECT_EQ(machine.memory()[4], 0xaaU);
  EXPECT_EQ(machine.memory()[7], 0xddU);
}

TEST(Rv32ThreadedYieldApi, hostMemoryWritePersistsOnOutOfGas)
{
  // ecall as the first instruction. The host writes 4 bytes at offset 4 through
  // view(), then reports OutOfGas. The headline semantic: the OOG trap fires
  // AND the host write persists -- there is NO per-ecall rollback.
  const std::vector<uint32_t> words = {0x00000073U};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(*program, /*gas=*/100);

  ThreadedMemoryHost host(ThreadedMemoryHost::Outcome::kOutOfGas);
  const StepResult result = machine.run(host);

  EXPECT_EQ(result, StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_TRUE(host.wrote);
  // The write happened before the OOG trap and is NOT rolled back.
  EXPECT_EQ(machine.memory()[4], 0xaaU);
  EXPECT_EQ(machine.memory()[7], 0xddU);
}

TEST(Rv32Threaded, placesInitialMemoryAtStartOffset)
{
  // lw x1, 8(x0); lw x2, 0(x0). No terminator: falls off the end into the
  // bad-pc sentinel, same shape as the run-and-report tests above.
  using test_program::load;
  const std::vector<uint32_t> words = {load(1, 2, 0, 8), load(2, 2, 0, 0)};
  const std::array<uint8_t, 4> stored = {0x78, 0x56, 0x34, 0x12};
  const auto created =
      ThreadedMachinePreInstantiation::create(words_to_bytes(words), stored,
                                              /*memory_start=*/8,
                                              /*total_memory_size=*/16,
                                              /*trace=*/false);
  ASSERT_TRUE(created.has_value());
  ThreadedMachine machine(*created, /*gas=*/100);

  RecordingHost host;
  const StepResult result = machine.run(host);
  EXPECT_EQ(result, StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(1), 0x12345678U); // image landed at VA 8
  EXPECT_EQ(machine.reg(2), 0U);          // below start is zero-filled
}

TEST(Rv32Threaded, createRejectsImagePastTotalMemory)
{
  const std::vector<uint32_t> words = {addi(1, 0, 5)};
  const std::array<uint8_t, 4> stored = {1, 2, 3, 4};
  EXPECT_EQ(ThreadedMachinePreInstantiation::create(words_to_bytes(words),
                                                    stored, /*memory_start=*/13,
                                                    /*total_memory_size=*/16,
                                                    /*trace=*/false)
                .error(),
            ProgramError::kMemoryTooLarge);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace
} // namespace hogd::rv32
