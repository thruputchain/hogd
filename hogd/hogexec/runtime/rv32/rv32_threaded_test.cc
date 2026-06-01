#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
      const Instr &t = threaded->code()[i];
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
  EXPECT_EQ(
      ThreadedProgram::create(std::vector<uint8_t>{0x13, 0x00, 0x00}).error(),
      ProgramError::kCodeSizeNotAligned);
  const std::vector<uint8_t> compressed = {0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(ThreadedProgram::create(compressed).error(),
            ProgramError::kUnsupportedCompressedInstruction);
  std::vector<uint8_t> csr(4);
  store_u32_le(csr, 0, 0x00001073U);
  EXPECT_EQ(ThreadedProgram::create(csr).error(),
            ProgramError::kUnsupportedSystemInstruction);
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

std::shared_ptr<const ThreadedProgram>
threaded_from_words(std::span<const uint32_t> words)
{
  auto prog = ThreadedProgram::create(words_to_bytes(words));
  EXPECT_TRUE(prog.has_value());
  return std::make_shared<const ThreadedProgram>(std::move(prog.value()));
}

TEST(Rv32Threaded, runsBasicArithmetic)
{
  // x1=5; x2=x1+7; x3=x1+x2. No terminator: falls off end -> bad-pc sentinel.
  const std::vector<uint32_t> words = {addi(1, 0, 5), addi(2, 1, 7),
                                       op(3, 1, 2, 0, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, /*memory_bytes=*/256, /*gas=*/100);

  RecordingHost host;
  const StepResult result = machine.run(host);

  EXPECT_EQ(result, StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(1), 5U);
  EXPECT_EQ(machine.reg(2), 12U);
  EXPECT_EQ(machine.reg(3), 17U);
  EXPECT_EQ(machine.reg(0), 0U);
}

TEST(Rv32Threaded, branchTakenSkipsInstruction)
{
  using test_program::branch;
  // x1=1, x2=1; beq x1,x2,+8 (taken) skips an addi that would set x3=99.
  const std::vector<uint32_t> words = {addi(1, 0, 1), addi(2, 0, 1),
                                       branch(0, 1, 2, 8), addi(3, 0, 99)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(3), 99U);
}

TEST(Rv32Threaded, jalLinksAndJumps)
{
  using test_program::j_type;
  // jal x1,+8 writes link (pc+4=4) to x1 and jumps over the addi at pc=4.
  // Use x4 for the skipped instruction (avoids x2=sp which is initialised to
  // the stack pointer by the machine).
  const std::vector<uint32_t> words = {j_type(1, 8), addi(4, 0, 7),
                                       addi(3, 0, 9)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
  // pc=0, nothing charged, and the body never executes.
  const std::vector<uint32_t> words = {addi(5, 0, 1), branch(0, 0, 0, 8),
                                       addi(6, 0, 99), addi(7, 0, 42)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, /*gas=*/1);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_EQ(machine.pc(), 0U);            // aborted at the run head, not the beq
  EXPECT_EQ(machine.gas_remaining(), 1U); // the run did not fit -> nothing debited
  EXPECT_EQ(machine.reg(5), 0U);          // body did not execute
}

TEST(Rv32Threaded, tightSelfLoopRunsOutOfGas)
{
  using test_program::branch;
  // beq x0,x0,0 branches to itself: an empty-body loop whose only gas is the
  // branch's own. The branch is its own run head, so enter_run charges its gas
  // every iteration and the loop terminates with kOutOfGas (rather than spinning
  // forever once the per-barrier self-charge is gone).
  const std::vector<uint32_t> words = {branch(0, 0, 0, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, /*gas=*/8);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_EQ(machine.gas_remaining(), 0U); // all 8 gas consumed, one per iteration
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
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.reg(4), 0xffffff80U); // lb: sign-extended
  EXPECT_EQ(machine.reg(5), 0x80U);       // lbu: zero-extended
}

TEST(Rv32Threaded, misalignedLoadTraps)
{
  using test_program::load;
  // x1=1; lw x4,0(x1) -> address 1, misaligned -> kMisalignedAccess.
  const std::vector<uint32_t> words = {addi(1, 0, 1), load(4, 2, 1, 0)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kMisalignedAccess);
}

TEST(Rv32Threaded, outOfBoundsStoreTraps)
{
  using test_program::store;
  // x1=0; sw x3, 2044(x1) -> address 2044 >= memory size 256 -> kBadMemory.
  // 2044 is 4-byte aligned so alignment check passes; bounds check fires.
  const std::vector<uint32_t> words = {addi(1, 0, 0), store(1, 3, 2044, 2)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
}

TEST(Rv32Threaded, ebreakTraps)
{
  const std::vector<uint32_t> words = {0x00100073U}; // ebreak
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBreakpoint);
}

TEST(Rv32Threaded, ecallHaltsWithA0)
{
  // x10(a0)=7, then ecall. RecordingHost default halts with a0.
  const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);
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
  ThreadedMachine machine(program, 256, 100);
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
    auto threaded = ThreadedProgram::create(bytes);
    ASSERT_TRUE(threaded.has_value()) << kind << " seed " << seed;
    auto tprog =
        std::make_shared<const ThreadedProgram>(std::move(threaded.value()));
    ThreadedMachine tmachine(tprog, memory_bytes, kCorpusGasBudget);
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
    auto strip_gas = [](ExecTrace t) {
      for (TraceStep &s : t.steps) {
        s.state.gas_remaining = 0;
        s.state_before.gas_remaining = 0;
      }
      return t;
    };
    const ExecTrace cstripped = strip_gas(ctrace);
    const ExecTrace tstripped = strip_gas(tobs.trace());

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

TEST(Rv32Threaded, untracedRunCompletes)
{
  // The zero-cost production path (run(host), untraced handlers) builds and
  // runs to the ecall halt.
  const std::vector<uint32_t> words =
      test_corpus::generate_register_only_program(123, 64);
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, /*memory_bytes=*/4096, kCorpusGasBudget);
  RecordingHost host;
  EXPECT_EQ(machine.run(host),
            StepResult::kHalted); // corpus ends in ecall
}

TEST(Rv32Threaded, nullProgramTrapsInvalidInstruction)
{
  // Matches canonical Machine: a null program is a construction trap of
  // kInvalidInstruction (a bad initial pc on a valid program is kBadPc).
  ThreadedMachine machine(nullptr, 256, 100);
  EXPECT_TRUE(machine.halted());
  EXPECT_EQ(machine.trap(), Trap::kInvalidInstruction);
  RecordingHost host;
  EXPECT_EQ(machine.run(host), StepResult::kTrap);
}

TEST(Rv32ThreadedYieldApi, startYieldsEcallThenResumeContinues)
{
  // a0=1 ; ecall ; a1=5 -> resume_from_ecall(continue_with(9)) writes a0, runs
  // on.
  const std::vector<uint32_t> words = {addi(10, 0, 1), 0x00000073U,
                                       addi(11, 0, 5)};
  auto program = threaded_from_words(words);
  ThreadedMachine machine(program, 256, 100);

  EXPECT_EQ(machine.start(), StepResult::kEcall);
  ASSERT_TRUE(machine.pending_ecall().has_value());
  EXPECT_EQ(machine.pending_ecall()->a0, 1U);

  const StepResult r = machine.resume_from_ecall(HostResult::continue_with(9));
  EXPECT_EQ(r, StepResult::kTrap); // falls off the end after addi a1
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  EXPECT_EQ(machine.reg(10), 9U);
  EXPECT_EQ(machine.reg(11), 5U);
}

TEST(Rv32ThreadedYieldApi, resumeFromEcallHaltAndTrapAreTerminal)
{
  {
    const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};
    auto program = threaded_from_words(words);
    ThreadedMachine machine(program, 256, 100);
    EXPECT_EQ(machine.start(), StepResult::kEcall);
    EXPECT_EQ(machine.resume_from_ecall(HostResult::halt(7)),
              StepResult::kHalted);
    EXPECT_EQ(machine.halt_code(), 7U);
  }
  {
    const std::vector<uint32_t> words = {0x00000073U};
    auto program = threaded_from_words(words);
    ThreadedMachine machine(program, 256, 100);
    EXPECT_EQ(machine.start(), StepResult::kEcall);
    EXPECT_EQ(machine.resume_from_ecall(HostResult::trap()), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kHostTrap);
  }
}

TEST(Rv32ThreadedYieldApi, startResumeMatchesRunForSameHost)
{
  // RecordingHost's default (kHaltWithA0) returns halt(view.reg(10)), so this
  // program (a0=7 ; ecall) halts with code 7 under both drivers.
  const std::vector<uint32_t> words = {addi(10, 0, 7), 0x00000073U};

  auto p1 = threaded_from_words(words);
  ThreadedMachine driven(p1, 256, 100);
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
  ThreadedMachine ran(p2, 256, 100);
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
  ThreadedMachine machine(program, /*memory_bytes=*/256, /*gas=*/100);

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
  ThreadedMachine machine(program, /*memory_bytes=*/256, /*gas=*/100);

  ThreadedMemoryHost host(ThreadedMemoryHost::Outcome::kOutOfGas);
  const StepResult result = machine.run(host);

  EXPECT_EQ(result, StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_TRUE(host.wrote);
  // The write happened before the OOG trap and is NOT rolled back.
  EXPECT_EQ(machine.memory()[4], 0xaaU);
  EXPECT_EQ(machine.memory()[7], 0xddU);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace
} // namespace hogd::rv32
