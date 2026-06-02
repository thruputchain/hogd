#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

// Standalone exercise of the portable RV32IM ISA core. This translation unit is
// linked against rv32_isa_core ONLY -- no rv32_vm, no OpenSSL, no hogd runtime
// (see CMakeLists.txt). It therefore serves two purposes:
//   1. a smoke test that decode + execute + the register/memory model + gas +
//      trap handling work in isolation, with no artifact/consensus/host layer;
//   2. a mechanical guardrail for the core's hogd-free boundary -- if the ISA
//      core ever starts referencing a symbol from outside rv32/, this
//      executable fails to link.
// Keep the dependencies here minimal on purpose; do not pull in rv32_vm,
// artifact, dispatch, or syscall headers.

namespace hogd::rv32 {
namespace {

using test_program::addi;
using test_program::load;
using test_program::make_program;
using test_program::op;
using test_program::store;

void
step_continuing(Machine &machine, int count)
{
  for (int i = 0; i < count; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue) << "step " << i;
  }
}

TEST(Rv32IsaCore, decodesAndExecutesArithmeticStandalone)
{
  // x1 = 5; x2 = 37; x3 = x1 + x2 = 42; write to x0 is discarded (hardwired 0).
  auto program = make_program({
      addi(1, 0, 5),
      addi(2, 0, 37),
      op(3, 1, 2, 0, 0),
      addi(0, 0, 99),
  });
  ASSERT_NE(program, nullptr);

  Machine machine(program, /*memory_bytes=*/1024, /*gas=*/100);
  step_continuing(machine, 4);

  EXPECT_EQ(machine.reg(1), 5U);
  EXPECT_EQ(machine.reg(2), 37U);
  EXPECT_EQ(machine.reg(3), 42U);
  EXPECT_EQ(machine.reg(0), 0U);           // x0 stays hardwired to zero.
  EXPECT_EQ(machine.pc(), 16U);            // Four 4-byte instructions retired.
  EXPECT_EQ(machine.gas_remaining(), 96U); // Four base-cost instructions.
  EXPECT_EQ(machine.trap(), Trap::kNone);
}

TEST(Rv32IsaCore, storeLoadRoundTripsThroughCoreMemory)
{
  // Store 42 at address 64, then load it back into a different register.
  auto program = make_program({
      addi(1, 0, 64),    // base address
      addi(2, 0, 42),    // value
      store(1, 2, 0, 2), // sw x2, 0(x1)
      load(3, 2, 1, 0),  // lw x3, 0(x1)
  });
  ASSERT_NE(program, nullptr);

  Machine machine(program, /*memory_bytes=*/1024, /*gas=*/100);
  step_continuing(machine, 4);

  EXPECT_EQ(machine.reg(3), 42U);
  EXPECT_EQ(machine.trap(), Trap::kNone);
}

TEST(Rv32IsaCore, outOfGasTrapsDeterministicallyStandalone)
{
  auto program = make_program({
      addi(1, 0, 1),
      addi(2, 0, 1),
      addi(3, 0, 1),
  });
  ASSERT_NE(program, nullptr);

  // Only enough gas for the first two instructions.
  Machine machine(program, /*memory_bytes=*/1024, /*gas=*/2);
  step_continuing(machine, 2);

  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
}

TEST(Rv32IsaCore, outOfBoundsStoreTrapsOnCoreMemoryModel)
{
  // Address 64 is past the 16-byte data memory, so the store must trap.
  auto program = make_program({
      addi(1, 0, 64),
      addi(2, 0, 1),
      store(1, 2, 0, 2),
  });
  ASSERT_NE(program, nullptr);

  Machine machine(program, /*memory_bytes=*/16, /*gas=*/100);
  step_continuing(machine, 2);

  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
}

} // namespace
} // namespace hogd::rv32
