#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/syscall_kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

// Correctness net for the calibration harness. A benchmark that measures the
// wrong thing reports a plausible number rather than failing, so the properties
// the estimator depends on are asserted here rather than assumed: that the
// kernels encode the instruction they claim, that they execute exactly the work
// they claim, that the production lowering leaves their bodies alone, and that
// the audit which checks that last point can actually fail.
namespace hogd::rv32::gas_calibration {
namespace {

constexpr uint32_t kTestBodySlots = 64;
constexpr uint32_t kTestTargets = 16;
constexpr uint32_t kTestTripCount = 4;

std::vector<KernelSpec>
representative_specs()
{
  std::vector<KernelSpec> specs;
  for (size_t index = 0; index < kOpCount; ++index) {
    const Op op = static_cast<Op>(index);
    if (!is_measurable_target(op)) {
      continue;
    }
    KernelSpec spec;
    spec.target = op;
    spec.body_slots = kTestBodySlots;
    spec.target_slots = kTestTargets;
    spec.trip_count = kTestTripCount;
    if (is_memory_target(op)) {
      spec.working_set = WorkingSet::kFullRange;
      specs.push_back(spec);
      spec.working_set = WorkingSet::kL1Region;
      specs.push_back(spec);
      if (op == Op::kLw) {
        spec.working_set = WorkingSet::kPointerChase;
        specs.push_back(spec);
      }
      continue;
    }
    if (is_divide_target(op)) {
      spec.context = OperandContext::kIndependent;
      spec.magnitude = OperandMagnitude::kLarge;
      specs.push_back(spec);
      spec.magnitude = OperandMagnitude::kSmall;
      specs.push_back(spec);
      spec.context = OperandContext::kChain;
      spec.magnitude = OperandMagnitude::kLarge;
      specs.push_back(spec);
      continue;
    }
    if (is_branch_target(op)) {
      spec.branch_taken = false;
      specs.push_back(spec);
      spec.branch_taken = true;
      specs.push_back(spec);
      continue;
    }
    spec.context = OperandContext::kIndependent;
    specs.push_back(spec);
    spec.context = OperandContext::kChain;
    specs.push_back(spec);
  }
  return specs;
}

// Every target slot must actually hold the opcode the spec asked for, decoded
// by the same decoder the engine uses. A hand-written encoding table is exactly
// the kind of thing that can be wrong in a way no timing result would reveal:
// a mis-encoded MULHSU would still run, still be timed, and still produce a
// confident cost -- for whatever instruction it really encoded.
TEST(Rv32GasCalibrationKernel, TargetSlotsEncodeTheRequestedOpcode)
{
  for (const KernelSpec &spec : representative_specs()) {
    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << op_name(spec.target);
    ASSERT_FALSE(kernel.value().target_slot_pcs.empty())
        << op_name(spec.target);

    const auto program = ThreadedProgram::create(kernel.value().code);
    ASSERT_TRUE(program.has_value()) << op_name(spec.target);
    for (const uint32_t pc : kernel.value().target_slot_pcs) {
      const auto decoded = program->instruction_at(pc / kInstructionBytes);
      ASSERT_TRUE(decoded.has_value()) << op_name(spec.target);
      EXPECT_EQ(decoded->op, spec.target)
          << "slot at pc " << pc << " decoded as " << op_name(decoded->op)
          << " but the spec asked for " << op_name(spec.target);
    }
  }
}

TEST(Rv32GasCalibrationKernel, JalTargetsRetireEverySlotWithAndWithoutLink)
{
  HaltHost host;
  for (const auto context : {OperandContext::kIndependent,
                             OperandContext::kNoLink}) {
    for (const bool saturated : {false, true}) {
      for (const uint32_t slots : {64U, 512U, 2048U}) {
        KernelSpec spec;
        spec.target = Op::kJal;
        spec.context = context;
        spec.body_slots = slots;
        spec.trip_count = kTestTripCount;
        spec.saturated = saturated;
        spec.target_slots = max_target_slots(slots, spec);
        const auto kernel = build_kernel(spec);
        ASSERT_TRUE(kernel.has_value());
        const auto decoded = ThreadedProgram::create(kernel->code);
        ASSERT_TRUE(decoded.has_value());
        for (const uint32_t pc : kernel->target_slot_pcs) {
          const auto instruction =
              decoded->instruction_at(pc / kInstructionBytes);
          ASSERT_TRUE(instruction.has_value());
          EXPECT_EQ(instruction->op, Op::kJal);
          EXPECT_EQ(instruction->immediate, int32_t{kInstructionBytes});
          if (context == OperandContext::kNoLink) {
            EXPECT_EQ(instruction->rd, 0);
          }
          else {
            EXPECT_GE(instruction->rd, kDataBankBase);
            EXPECT_LT(instruction->rd, kDataBankBase + kDataBankSize);
          }
        }
        for (const bool fuse : {false, true}) {
          const auto audit = audit_kernel(*kernel, spec, fuse);
          EXPECT_TRUE(audit.clean());
          EXPECT_EQ(audit.body_runs, spec.target_slots + 1);
          auto pre = ThreadedMachinePreInstantiation::create(
              kernel->code, kernel->initial_memory, 0, kernel->memory_bytes,
              /*trace=*/false, kMaxRunGas, fuse);
          ASSERT_TRUE(pre.has_value());
          ThreadedMachine machine(*pre, kGasLimit);
          const RunOutcome outcome = run_once(machine, host);
          EXPECT_EQ(outcome.result, StepResult::kHalted);
          EXPECT_EQ(outcome.trap, Trap::kNone);
          EXPECT_EQ(outcome.gas_consumed, kernel->scheduled_gas);
        }
      }
    }
  }
  // Adding a timing kernel must not silently change the fusion fingerprint.
  EXPECT_FALSE(canonical_instruction_word(Op::kJal, kDataBankBase).has_value());
}

TEST(Rv32GasCalibrationKernel, FullRangeVisitsAllBanksAndCacheLines)
{
  for (const Op op : {Op::kLb, Op::kLh, Op::kLw, Op::kLbu, Op::kLhu, Op::kSb,
                      Op::kSh, Op::kSw}) {
    for (const uint32_t targets : {254U, 510U, 512U, 600U}) {
      KernelSpec spec;
      spec.target = op;
      spec.working_set = WorkingSet::kFullRange;
      spec.saturated = true;
      spec.body_slots = targets + 2;
      spec.target_slots = targets;
      const auto kernel = build_kernel(spec);
      ASSERT_TRUE(kernel.has_value());
      const auto program = ThreadedProgram::create(kernel->code);
      ASSERT_TRUE(program.has_value());
      std::set<uint32_t> lines;
      std::set<uint8_t> banks;
      for (const uint32_t pc : kernel->target_slot_pcs) {
        const auto instruction =
            program->instruction_at(pc / kInstructionBytes);
        ASSERT_TRUE(instruction.has_value());
        const uint8_t bank = instruction->rs1 - kAddressBankBase;
        ASSERT_LT(bank, kAddressBankSize);
        const int32_t address = 2048 + (bank * 8192) + instruction->imm;
        ASSERT_GE(address, 0);
        ASSERT_LT(static_cast<uint32_t>(address), kDefaultMaxMemoryBytes);
        lines.insert(static_cast<uint32_t>(address) / 64);
        banks.insert(bank);
      }
      EXPECT_EQ(banks.size(), 8U);
      EXPECT_EQ(lines.size(), targets < 512 ? targets : 512U);
    }
  }
}

TEST(Rv32GasCalibrationMeasurement, RejectsWrongGasAndRecoversOnRemeasurement)
{
  KernelSpec spec;
  spec.trip_count = 4;
  const auto kernel = build_kernel(spec);
  ASSERT_TRUE(kernel.has_value());
  const auto program =
      ThreadedMachinePreInstantiation::create(kernel->code,
                                              kernel->initial_memory, 0,
                                              kernel->memory_bytes, false);
  ASSERT_TRUE(program.has_value());
  HaltHost host;
  std::vector<MeasurementPoint> points{
      {.program = &*program,
       .trip_count = spec.trip_count,
       .expected_gas = kernel->scheduled_gas + 1}};
  measure_interleaved(points, host, {.repeats = 3, .warmup_runs = 0});
  EXPECT_FALSE(points[0].ran_clean);
  EXPECT_TRUE(points[0].samples.empty());
  points[0].expected_gas = kernel->scheduled_gas;
  measure_interleaved(points, host, {.repeats = 3, .warmup_runs = 0});
  EXPECT_TRUE(points[0].ran_clean);
  EXPECT_EQ(points[0].samples.size(), 3U);
}

TEST(Rv32GasCalibrationWorkloads, RequiresRepeatedSuccessfulMeasurements)
{
  EXPECT_THROW((void)measure_workloads(0, 0, 1000.0), std::runtime_error);
  EXPECT_THROW((void)measure_workloads(2, 0, 1000.0), std::runtime_error);
  const auto workloads = measure_workloads(3, 0, 1000.0);
  ASSERT_EQ(workloads.size(), 11U);
  for (const auto &workload : workloads) {
    EXPECT_EQ(workload.samples, 3U);
    EXPECT_GT(workload.scheduled_gas_per_iteration, 0U);
  }
}

TEST(Rv32GasCalibrationKernel, PlacesRequestedTargetsNonAdjacentlyInTheInterior)
{
  for (const KernelSpec &spec : representative_specs()) {
    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << op_name(spec.target);
    EXPECT_EQ(kernel.value().target_slot_pcs.size(), spec.target_slots)
        << op_name(spec.target);

    const uint32_t first_slot =
        kernel.value().body_start_pc / kInstructionBytes;
    const uint32_t last_slot = first_slot + spec.body_slots - 1;
    uint32_t previous_slot = 0;
    bool have_previous = false;
    for (const uint32_t pc : kernel.value().target_slot_pcs) {
      const uint32_t slot = pc / kInstructionBytes;
      // Slot 0 and the last slot stay filler so the prologue/body and
      // body/loop-control boundaries are the same instruction pair in every
      // kernel of a sweep.
      EXPECT_GT(slot, first_slot) << op_name(spec.target);
      EXPECT_LT(slot, last_slot) << op_name(spec.target);
      if (have_previous) {
        EXPECT_GT(slot, previous_slot + 1)
            << "adjacent targets at " << previous_slot << " and " << slot
            << " would expose the pair to fusion";
      }
      previous_slot = slot;
      have_previous = true;
    }
  }
}

// Placement phase must move work around without changing how much there is.
// If it changed the work, spreading a sweep point over several phases would
// average over different quantities rather than over layout luck.
TEST(Rv32GasCalibrationKernel, PlacementPhaseMovesWorkWithoutChangingIt)
{
  KernelSpec spec;
  spec.target = Op::kMul;
  spec.body_slots = kTestBodySlots;
  spec.target_slots = kTestTargets;
  spec.trip_count = kTestTripCount;

  const auto first = build_kernel(spec);
  ASSERT_TRUE(first.has_value());
  bool saw_different_placement = false;
  for (uint32_t phase = 1; phase < 6; ++phase) {
    spec.placement_phase = phase;
    const auto other = build_kernel(spec);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other.value().scheduled_gas, first.value().scheduled_gas);
    EXPECT_EQ(other.value().dynamic_instructions,
              first.value().dynamic_instructions);
    EXPECT_EQ(other.value().target_slot_pcs.size(),
              first.value().target_slot_pcs.size());
    if (other.value().target_slot_pcs != first.value().target_slot_pcs) {
      saw_different_placement = true;
    }
  }
  EXPECT_TRUE(saw_different_placement)
      << "phase never changed placement, so it cannot average layout bias";
}

// The strongest single check in the harness: running a kernel must consume
// exactly the gas the schedule says its instruction sequence costs. That pins
// the retired instruction count, the loop trip count, and the absence of any
// trap or early exit in one assertion -- if a kernel silently ran a different
// number of instructions than the harness credits it with, every ns-per
// instruction derived from it would be wrong by that ratio.
// A saturated body must really be all target, and must run.
//
// The saturated measurement divides a kernel's time by its instruction count
// and calls the result the per-instruction cost of a program made of that
// opcode. That claim rests on two things the harness controls: every interior
// slot actually holds the target, and the kernel actually executes all of it.
// Check both against the production decoder and the engine's own gas
// accounting, for every opcode that can be saturated.
TEST(Rv32GasCalibrationKernel, NoBodySlotEncodesAnImmediateItCannotReach)
{
  // Every body-slot encoder masks its immediate to the field width, so an
  // out-of-range offset does not fail -- it encodes a different instruction
  // that runs and halts cleanly. That is how the loop back edge truncated
  // silently; JALR has the same shape, because its base register is fixed at
  // the body midpoint and the immediate carries the whole per-slot offset.
  //
  // Build every target at every body size the harness allows. A kernel must
  // either come out correct or be refused by name -- never come out wrong.
  HaltHost host;
  size_t refused = 0;
  for (const uint32_t body_slots : {8U, 64U, 512U, 1024U, 2048U, 8192U}) {
    for (size_t index = 0; index < kOpCount; ++index) {
      const Op op = static_cast<Op>(index);
      if (!is_measurable_target(op)) {
        continue;
      }
      KernelSpec spec;
      spec.target = op;
      spec.saturated = true;
      spec.body_slots = body_slots;
      spec.trip_count = kTestTripCount;
      spec.target_slots = max_target_slots(body_slots, spec);

      const auto kernel = build_kernel(spec);
      if (!kernel.has_value()) {
        // Refusal is the acceptable outcome; a wrong encoding is not.
        ++refused;
        continue;
      }
      const auto program = ThreadedProgram::create(kernel.value().code);
      ASSERT_TRUE(program.has_value()) << op_name(op) << " @ " << body_slots;
      for (const uint32_t pc : kernel.value().target_slot_pcs) {
        const auto decoded = program->instruction_at(pc / kInstructionBytes);
        ASSERT_TRUE(decoded.has_value()) << op_name(op) << " @ " << body_slots;
        EXPECT_EQ(decoded->op, op) << op_name(op) << " @ " << body_slots;
      }
    }
  }
  // JALR above 1024 slots is the case that motivated this; if nothing is ever
  // refused the loop is not reaching the range it exists to probe.
  EXPECT_GT(refused, 0U);
}

TEST(Rv32GasCalibrationKernel, SaturatedPairsTileWithoutOverlapping)
{
  // A saturated pair body has to be the pair repeated -- sw lw sw lw -- not a
  // run of stores. Each substitution occupies two slots, so placing a head at
  // every interior slot makes each pair's first instruction land on the
  // previous pair's second: the body collapses to all-first-component with a
  // single tail at the end, and it audits clean because sw;sw is not a fusion
  // rule. The pair's cost would then be measured on a program that contains
  // essentially none of the interaction the row exists to measure.
  for (const PairKind pair :
       {PairKind::kStoreLoadForward, PairKind::kStoreLoadDistinct}) {
    KernelSpec spec;
    spec.pair = pair;
    spec.saturated = true;
    spec.body_slots = kTestBodySlots;
    spec.trip_count = kTestTripCount;
    spec.target_slots = max_target_slots(spec.body_slots, spec);

    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << pair_name(pair);

    const auto [head_op, tail_op] = pair_components(pair);
    const auto program = ThreadedProgram::create(kernel.value().code);
    ASSERT_TRUE(program.has_value()) << pair_name(pair);

    // Every recorded slot alternates head, tail, head, tail: two slots per
    // substitution, in program order, none of them overwritten.
    const auto &pcs = kernel.value().target_slot_pcs;
    ASSERT_EQ(pcs.size() % 2U, 0U) << pair_name(pair);
    ASSERT_GT(pcs.size(), 2U) << pair_name(pair);
    for (size_t i = 0; i < pcs.size(); i += 2) {
      EXPECT_EQ(pcs[i + 1], pcs[i] + kInstructionBytes)
          << pair_name(pair) << ": pair halves are not adjacent";
      const auto head = program->instruction_at(pcs[i] / kInstructionBytes);
      const auto tail = program->instruction_at(pcs[i + 1] / kInstructionBytes);
      ASSERT_TRUE(head.has_value() && tail.has_value()) << pair_name(pair);
      EXPECT_EQ(head->op, head_op) << pair_name(pair) << " at pc " << pcs[i];
      EXPECT_EQ(tail->op, tail_op)
          << pair_name(pair) << " at pc " << pcs[i + 1];
    }

    // And the body really is made of the pair: at least a third of it, which a
    // body of stores with one trailing load could never reach.
    EXPECT_GE(kernel.value().target_units, spec.body_slots / 3)
        << pair_name(pair);
    EXPECT_EQ(pcs.size(), size_t{kernel.value().target_units} * 2U)
        << pair_name(pair);
  }
}

TEST(Rv32GasCalibrationKernel, SaturatedBodiesAreAllTargetAndRunFully)
{
  HaltHost host;
  size_t measured = 0;
  size_t fused = 0;
  for (size_t index = 0; index < kOpCount; ++index) {
    const Op op = static_cast<Op>(index);
    if (!is_measurable_target(op)) {
      continue;
    }
    KernelSpec spec;
    spec.target = op;
    spec.saturated = true;
    spec.body_slots = kTestBodySlots;
    spec.trip_count = kTestTripCount;
    spec.target_slots = max_target_slots(spec.body_slots, spec);

    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << op_name(op);
    // Every interior slot is a target: the whole point of saturation.
    EXPECT_EQ(kernel.value().target_slot_pcs.size(), spec.body_slots - 2)
        << op_name(op);

    if (!audit_kernel(kernel.value(), spec).clean()) {
      // Adjacent copies of this opcode fuse, so it cannot be saturated. The
      // driver reports that rather than measuring the fused form; here just
      // record that it is a minority case.
      ++fused;
      continue;
    }
    ++measured;

    const auto program = ThreadedProgram::create(kernel.value().code);
    ASSERT_TRUE(program.has_value()) << op_name(op);
    for (const uint32_t pc : kernel.value().target_slot_pcs) {
      const auto decoded = program->instruction_at(pc / kInstructionBytes);
      ASSERT_TRUE(decoded.has_value()) << op_name(op);
      EXPECT_EQ(decoded->op, op) << "saturated body slot at pc " << pc
                                 << " decoded as " << op_name(decoded->op);
    }

    auto pre =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(pre.has_value()) << op_name(op);
    ThreadedMachine machine(*pre, kGasLimit);
    const RunOutcome outcome = run_once(machine, host);
    EXPECT_EQ(outcome.result, StepResult::kHalted) << op_name(op);
    EXPECT_EQ(outcome.gas_consumed, kernel.value().scheduled_gas)
        << op_name(op) << " did not run its whole saturated body";
  }
  // Saturation has to work for most of the ISA or the primary estimator has no
  // coverage. Only opcodes that pair with themselves under a fusion rule are
  // expected to decline.
  EXPECT_GT(measured, 35U);
  EXPECT_LE(fused, 3U);
}

// A kernel must run all of its body, at every body size the harness allows.
//
// This is what a silently-truncated loop breaks. The old backwards branch
// overflowed its 13-bit immediate past 1024 slots and the encoder truncated
// rather than refused: at 2048 slots the kernel reported a clean halt having
// burned 2107 of 16443 credited gas, every opcode priced at the baseline, and
// the sweep looked reproducible because it had stopped measuring. Comparing
// consumed against credited gas catches that in one assertion, so size the
// bodies past the old cliff.
TEST(Rv32GasCalibrationKernel, RunsTheWholeBodyAtEveryBodySize)
{
  HaltHost host;
  for (const uint32_t slots :
       {64U, 256U, 512U, 768U, 1024U, 1536U, 2048U, 4096U, 8192U}) {
    KernelSpec spec;
    spec.target = Op::kLw;
    spec.working_set = WorkingSet::kPointerChase;
    spec.body_slots = slots;
    spec.trip_count = 8;
    spec.target_slots = max_target_slots(slots, spec);

    const auto kernel = build_kernel(spec);
    // Refusing to build is a fine answer; building something that does not run
    // is not.
    if (!kernel.has_value()) {
      continue;
    }
    auto program =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(program.has_value()) << "body_slots " << slots;

    ThreadedMachine machine(*program, kGasLimit);
    const RunOutcome outcome = run_once(machine, host);
    EXPECT_EQ(outcome.result, StepResult::kHalted)
        << "body_slots " << slots << " did not halt (trap "
        << static_cast<int>(outcome.trap) << ")";
    EXPECT_EQ(outcome.trap, Trap::kNone) << "body_slots " << slots;
    EXPECT_EQ(outcome.gas_consumed, kernel.value().scheduled_gas)
        << "body_slots " << slots << " executed " << outcome.gas_consumed
        << " gas but is credited with " << kernel.value().scheduled_gas
        << " -- the loop is not running the whole body";
  }
}

TEST(Rv32GasCalibrationKernel, RunsExactlyTheWorkItIsCreditedWith)
{
  HaltHost host;
  for (const KernelSpec &spec : representative_specs()) {
    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << op_name(spec.target);

    auto program =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(program.has_value()) << op_name(spec.target);

    ThreadedMachine machine(*program, kGasLimit);
    const RunOutcome outcome = run_once(machine, host);
    EXPECT_EQ(outcome.result, StepResult::kHalted)
        << op_name(spec.target) << " trapped with "
        << static_cast<int>(outcome.trap);
    EXPECT_EQ(outcome.trap, Trap::kNone) << op_name(spec.target);
    EXPECT_EQ(outcome.gas_consumed, kernel.value().scheduled_gas)
        << op_name(spec.target) << " (" << context_name(spec.context)
        << ") ran a different amount of work than the harness credits it with";
  }
}

// The measurement assumes each body slot is one dispatch of one instruction.
// The threaded lowering fuses nine patterns, so that assumption is a property
// of the generated code, not a given.
TEST(Rv32GasCalibrationKernel, SweepBodiesLowerWithoutFusion)
{
  for (const KernelSpec &spec : representative_specs()) {
    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << op_name(spec.target);
    const LoweringAudit audit = audit_kernel(kernel.value(), spec);
    EXPECT_TRUE(audit.clean())
        << op_name(spec.target) << " (" << context_name(spec.context) << "/"
        << working_set_name(spec.working_set) << "): lowering rewrote "
        << audit.rewritten_slots << " slot(s), first at "
        << audit.first_rewritten_slot;
    EXPECT_LT(audit.max_run_gas_seen, kMaxRunGas)
        << op_name(spec.target)
        << ": run gas near the cap means the lowering is about to insert "
           "break carriers, which change handlers mid-sweep";
  }
}

// A clean audit is only evidence if a dirty one is possible. addi;addi is one
// of the ALU-ALU fusion pairs, so a body built from it must come back dirty --
// otherwise the check above is passing vacuously and proves nothing.
TEST(Rv32GasCalibrationAudit, DetectsFusionWhenItHappens)
{
  std::vector<uint32_t> words;
  emit_prologue(words, WorkingSet::kL1Region, kTestTripCount);
  const size_t body_start_index = words.size();
  constexpr size_t kFusibleBodySlots = 32;
  for (size_t i = 0; i < kFusibleBodySlots; ++i) {
    words.push_back(test_program::addi(kDataBankBase, kDataBankBase, 1));
  }
  words.push_back(filler_instruction_word(0));
  words.push_back(0x00000073U);

  const std::vector<uint8_t> code = test_program::code_from_words(words);
  const std::vector<uint8_t> memory(kDefaultMaxMemoryBytes, 0);
  const LoweringAudit audit = audit_body(code, memory, kDefaultMaxMemoryBytes,
                                         body_start_index, kFusibleBodySlots);
  EXPECT_FALSE(audit.clean());
  EXPECT_GT(audit.rewritten_slots, 0U)
      << "the audit did not notice addi;addi fusion, so a clean audit on a "
         "calibration kernel means nothing";
}

TEST(Rv32GasCalibrationAudit, CountsOneRunPerBarrier)
{
  KernelSpec spec;
  spec.target = Op::kBne;
  spec.body_slots = kTestBodySlots;
  spec.target_slots = kTestTargets;
  spec.trip_count = kTestTripCount;
  const auto kernel = build_kernel(spec);
  ASSERT_TRUE(kernel.has_value());
  const LoweringAudit audit = audit_kernel(kernel.value(), spec);
  // One run to start, plus one more after each branch in the body.
  EXPECT_EQ(audit.body_runs, kTestTargets + 1);

  KernelSpec straight_line = spec;
  straight_line.target = Op::kAdd;
  straight_line.branch_taken = false;
  const auto straight = build_kernel(straight_line);
  ASSERT_TRUE(straight.has_value());
  EXPECT_EQ(audit_kernel(straight.value(), straight_line).body_runs, 1U);
}

// The chase is only a latency test if its addresses are unpredictable and it
// actually visits the whole space. A seeding bug that produced a short cycle,
// or one node pointing at itself, would still run and still be timed -- and
// would quietly measure an L1 hit instead of the memory latency it claims.
TEST(Rv32GasCalibrationKernel, PointerChaseVisitsEveryNodeExactlyOnce)
{
  KernelSpec spec;
  spec.target = Op::kLw;
  spec.working_set = WorkingSet::kPointerChase;
  spec.body_slots = kTestBodySlots;
  spec.target_slots = kTestTargets;
  spec.trip_count = kTestTripCount;
  const auto kernel = build_kernel(spec);
  ASSERT_TRUE(kernel.has_value());

  const auto read_word = [&](uint32_t address) {
    return uint32_t{kernel.value().initial_memory[address]} |
           (uint32_t{kernel.value().initial_memory[address + 1]} << 8) |
           (uint32_t{kernel.value().initial_memory[address + 2]} << 16) |
           (uint32_t{kernel.value().initial_memory[address + 3]} << 24);
  };

  constexpr uint32_t kStride = 64;
  const uint32_t nodes = kernel.value().memory_bytes / kStride;
  std::vector<bool> visited(nodes, false);
  uint32_t address = 0;
  uint32_t steps = 0;
  uint32_t adjacent_hops = 0;
  for (; steps < nodes; ++steps) {
    ASSERT_LT(address, kernel.value().memory_bytes);
    ASSERT_EQ(address % kStride, 0U);
    const uint32_t node = address / kStride;
    ASSERT_FALSE(visited[node]) << "cycle closed early at node " << node;
    visited[node] = true;
    const uint32_t next = read_word(address);
    if (next == address + kStride) {
      ++adjacent_hops;
    }
    address = next;
  }
  EXPECT_EQ(steps, nodes) << "the chase does not cover the whole space";
  EXPECT_EQ(address, 0U) << "the chase is not a single cycle";
  // A sequential walk is prefetchable and would measure the prefetcher, not
  // the load. A shuffle leaves only a handful of adjacent hops by chance.
  EXPECT_LT(adjacent_hops, nodes / 8)
      << "chase order looks sequential enough to be prefetched";
}

// The fingerprint's job is to tell "same engine, different machine" from
// "different engine", so it has to depend on the engine and on nothing else.
TEST(Rv32GasCalibrationProvenance, EngineFingerprintIsStable)
{
  const std::string first = engine_fingerprint();
  EXPECT_EQ(first.size(), 16U);
  EXPECT_EQ(first, engine_fingerprint());
  EXPECT_NE(first, std::string(16, '0'));
}

// A fingerprint is only useful if the corpus it hashes can actually observe the
// lowering's fusion rules. The calibration kernels use operands chosen so that
// nothing fuses; a fingerprint corpus built the same way would probe a lowering
// in which no rule can fire and would sit still when one changed. Assert the
// canonical corpus contains at least one pair that really does fuse, so the
// fingerprint has something to be sensitive to.
TEST(Rv32GasCalibrationProvenance, CanonicalCorpusCanFuse)
{
  // lui rd, hi ; addi rd, rd, lo -- the same-register pair fuse_lui_addi wants.
  const auto lui = canonical_instruction_word(Op::kLui, kDataBankBase);
  const auto addi = canonical_instruction_word(Op::kAddi, kDataBankBase);
  ASSERT_TRUE(lui.has_value());
  ASSERT_TRUE(addi.has_value());

  const std::vector<uint8_t> paired = test_program::code_from_words(
      {filler_instruction_word(0), lui.value(), addi.value(),
       filler_instruction_word(1), 0x00000073U});
  const std::vector<uint8_t> memory(64, 0);
  const LoweringAudit audit = audit_body(paired, memory, 64, 1, 2);
  EXPECT_GT(audit.rewritten_slots, 0U)
      << "the canonical corpus cannot express a fusible pair, so the engine "
         "fingerprint would not notice a fusion rule changing";
}

// The calibration kernels must have the opposite property.
TEST(Rv32GasCalibrationProvenance, CalibrationOperandsDoNotFuse)
{
  KernelSpec spec;
  spec.target = Op::kAddi;
  spec.body_slots = kTestBodySlots;
  spec.target_slots = kTestTargets;
  spec.trip_count = kTestTripCount;
  const auto kernel = build_kernel(spec);
  ASSERT_TRUE(kernel.has_value());
  EXPECT_TRUE(audit_kernel(kernel.value(), spec).clean())
      << "ADDI is a fusion head in four rules; if the kernel's operands let "
         "any of them fire, its slope is not a per-instruction cost";
}

// A pair substitution has to keep every property a single-opcode one has:
// its two slots land inside the interior, pairs never touch each other (the
// trailing load of one beside the leading store of the next would fuse), and
// the lowering leaves all of it alone. The cap and the placement span are
// computed in two different files, and when they disagreed by one the densest
// kernel did exactly that and the audit caught it -- so the cap is asserted
// here rather than trusted.
TEST(Rv32GasCalibrationKernel, PairsStayApartAndUnfused)
{
  for (const PairKind kind :
       {PairKind::kStoreLoadForward, PairKind::kStoreLoadDistinct}) {
    KernelSpec spec;
    spec.pair = kind;
    spec.working_set = WorkingSet::kL1Region;
    spec.body_slots = kTestBodySlots;
    spec.trip_count = kTestTripCount;
    spec.target_slots = max_target_slots(kTestBodySlots, spec);
    ASSERT_GT(spec.target_slots, 0U) << pair_name(kind);

    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << pair_name(kind);
    // Two recorded slots per pair: both halves are audited, not just the head.
    EXPECT_EQ(kernel.value().target_slot_pcs.size(), spec.target_slots * 2)
        << pair_name(kind);

    const uint32_t first_slot =
        kernel.value().body_start_pc / kInstructionBytes;
    const uint32_t last_slot = first_slot + spec.body_slots - 1;
    std::vector<uint32_t> slots;
    for (const uint32_t pc : kernel.value().target_slot_pcs) {
      slots.push_back(pc / kInstructionBytes);
    }
    for (const uint32_t slot : slots) {
      EXPECT_GT(slot, first_slot) << pair_name(kind);
      EXPECT_LT(slot, last_slot) << pair_name(kind);
    }
    // Slots come in adjacent couples, and consecutive couples never touch.
    for (size_t i = 0; i + 1 < slots.size(); i += 2) {
      EXPECT_EQ(slots[i + 1], slots[i] + 1) << pair_name(kind);
      if (i + 2 < slots.size()) {
        EXPECT_GT(slots[i + 2], slots[i + 1] + 1)
            << "pairs at " << slots[i] << " and " << slots[i + 2]
            << " touch, so the leading store fuses with the trailing load";
      }
    }
    EXPECT_TRUE(audit_kernel(kernel.value(), spec).clean()) << pair_name(kind);
  }
}

// pair_components must name the opcodes the kernel actually places.
//
// Every consumer that reasons about a pair in terms of its parts trusts this:
// the gas it is credited with, and the worst-case ceiling its interaction is
// checked against. Both of those used to be written as a literal SW + LW,
// which was correct until auipc;jalr arrived and silently inherited a bound
// belonging to two opcodes it does not contain. The four opcodes involved all
// cost the same gas today, so the accounting test could not see the error --
// only decoding the body can. Assert it against the same decoder the engine
// uses, so a new PairKind cannot be added without this being right.
TEST(Rv32GasCalibrationKernel, PairComponentsMatchTheEmittedBody)
{
  for (const PairKind kind :
       {PairKind::kStoreLoadForward, PairKind::kStoreLoadDistinct,
        PairKind::kAuipcJalr}) {
    KernelSpec spec;
    spec.pair = kind;
    spec.working_set = WorkingSet::kL1Region;
    spec.body_slots = kTestBodySlots;
    spec.trip_count = kTestTripCount;
    spec.target_slots = max_target_slots(kTestBodySlots, spec);
    ASSERT_GT(spec.target_slots, 0U) << pair_name(kind);

    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << pair_name(kind);
    const auto program = ThreadedProgram::create(kernel.value().code);
    ASSERT_TRUE(program.has_value()) << pair_name(kind);

    const auto [first, second] = pair_components(kind);
    const std::vector<uint32_t> &pcs = kernel.value().target_slot_pcs;
    ASSERT_EQ(pcs.size() % 2, 0U) << pair_name(kind);
    for (size_t i = 0; i + 1 < pcs.size(); i += 2) {
      const auto head = program->instruction_at(pcs[i] / kInstructionBytes);
      const auto tail = program->instruction_at(pcs[i + 1] / kInstructionBytes);
      ASSERT_TRUE(head.has_value()) << pair_name(kind);
      ASSERT_TRUE(tail.has_value()) << pair_name(kind);
      EXPECT_EQ(head->op, first)
          << pair_name(kind) << " head decoded as " << op_name(head->op)
          << " but pair_components says " << op_name(first);
      EXPECT_EQ(tail->op, second)
          << pair_name(kind) << " tail decoded as " << op_name(tail->op)
          << " but pair_components says " << op_name(second);
    }
  }
}

// One more target than the cap allows must be refused, not silently placed
// somewhere that fuses.
TEST(Rv32GasCalibrationKernel, PairCapIsEnforced)
{
  KernelSpec spec;
  spec.pair = PairKind::kStoreLoadForward;
  spec.working_set = WorkingSet::kL1Region;
  spec.body_slots = kTestBodySlots;
  spec.trip_count = kTestTripCount;
  spec.target_slots = max_target_slots(kTestBodySlots, spec) + 1;
  EXPECT_FALSE(build_kernel(spec).has_value());
}

// The pair kernels must run and be charged exactly, like every other kernel:
// the gas accounting was rewritten to decode the body when pairs made the
// old slot-walking version wrong.
TEST(Rv32GasCalibrationKernel, PairKernelsRunTheWorkTheyAreCreditedWith)
{
  HaltHost host;
  for (const PairKind kind :
       {PairKind::kStoreLoadForward, PairKind::kStoreLoadDistinct}) {
    KernelSpec spec;
    spec.pair = kind;
    spec.working_set = WorkingSet::kL1Region;
    spec.body_slots = kTestBodySlots;
    spec.trip_count = kTestTripCount;
    spec.target_slots = max_target_slots(kTestBodySlots, spec);

    const auto kernel = build_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << pair_name(kind);
    auto program =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(program.has_value()) << pair_name(kind);

    ThreadedMachine machine(*program, kGasLimit);
    const RunOutcome outcome = run_once(machine, host);
    EXPECT_EQ(outcome.result, StepResult::kHalted) << pair_name(kind);
    EXPECT_EQ(outcome.trap, Trap::kNone) << pair_name(kind);
    EXPECT_EQ(outcome.gas_consumed, kernel.value().scheduled_gas)
        << pair_name(kind);
  }
}

// ---------------------------------------------------------------------------
// Syscall seam kernels.
//
// The seam measurement's denominator is a count of host calls, and nothing
// about a wrong count looks wrong: a body that placed fewer ecalls than it was
// asked for still runs, still halts, and still produces a confident ns/call --
// for a call rate it never achieved. So the properties the estimator rests on
// are asserted here: the calls are placed, they are all reached, the lowering
// leaves them alone, and each one really is its own straight-line run.

constexpr uint32_t kSyscallTestBodySlots = 64;
constexpr uint32_t kSyscallTestTripCount = 4;

// Every body slot that should hold an ecall does, decoded by the production
// decoder rather than by trusting the encoder that wrote it.
TEST(Rv32GasCalibrationSyscall, PlacesTheCallsItClaims)
{
  for (const SyscallShape shape : kAllSyscallShapes) {
    for (const uint32_t groups :
         {0U, 1U, 7U, max_call_groups(kSyscallTestBodySlots, shape)}) {
      SyscallKernelSpec spec;
      spec.shape = shape;
      spec.body_slots = kSyscallTestBodySlots;
      spec.call_groups = groups;
      spec.trip_count = kSyscallTestTripCount;
      const auto kernel = build_syscall_kernel(spec);
      ASSERT_TRUE(kernel.has_value()) << syscall_shape_name(shape);
      EXPECT_EQ(kernel.value().calls_per_iteration, groups)
          << syscall_shape_name(shape);

      const auto program = ThreadedProgram::create(kernel.value().code);
      ASSERT_TRUE(program.has_value()) << syscall_shape_name(shape);
      const size_t body_start =
          kernel.value().body_start_pc / kInstructionBytes;
      size_t ecalls = 0;
      for (size_t slot = 0; slot < spec.body_slots; ++slot) {
        const auto decoded = program->instruction_at(body_start + slot);
        ASSERT_TRUE(decoded.has_value()) << syscall_shape_name(shape);
        if (decoded->op == Op::kEcall) {
          ++ecalls;
        }
      }
      EXPECT_EQ(ecalls, groups)
          << syscall_shape_name(shape) << " body holds " << ecalls
          << " ecalls but is credited with " << groups << " calls";
    }
  }
}

// The saturated body is the primary estimator, so it has to be genuinely full:
// a body one group short would be timed against a denominator it never ran.
TEST(Rv32GasCalibrationSyscall, SaturatedBodiesFillTheInterior)
{
  for (const SyscallShape shape : kAllSyscallShapes) {
    SyscallKernelSpec spec;
    spec.shape = shape;
    spec.body_slots = kSyscallTestBodySlots;
    spec.saturated = true;
    spec.trip_count = kSyscallTestTripCount;
    const auto kernel = build_syscall_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << syscall_shape_name(shape);
    const uint32_t cap = max_call_groups(kSyscallTestBodySlots, shape);
    EXPECT_EQ(kernel.value().calls_per_iteration, cap)
        << syscall_shape_name(shape);
    // The interior is every slot but the two fixed filler boundaries, and a
    // saturated body leaves none of it unused.
    EXPECT_EQ(cap * syscall_shape_slots(shape), kSyscallTestBodySlots - 2)
        << syscall_shape_name(shape);
  }
}

// The count the harness divides by has to be the count the run actually made.
// This is the check that would have caught a group silently overwriting its
// neighbour's ecall, and it is exact rather than statistical.
TEST(Rv32GasCalibrationSyscall, MakesExactlyTheCallsItIsCreditedWith)
{
  for (const SyscallShape shape : kAllSyscallShapes) {
    SyscallKernelSpec spec;
    spec.shape = shape;
    spec.body_slots = kSyscallTestBodySlots;
    spec.saturated = true;
    spec.trip_count = kSyscallTestTripCount;
    const auto kernel = build_syscall_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << syscall_shape_name(shape);

    auto program =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(program.has_value()) << syscall_shape_name(shape);

    ThreadedMachine machine(*program, kGasLimit);
    SyscallDispatchHost host(shape);
    const RunOutcome outcome = run_once(machine, host);
    EXPECT_EQ(outcome.result, StepResult::kHalted)
        << syscall_shape_name(shape) << " trapped with "
        << static_cast<int>(outcome.trap);
    EXPECT_EQ(host.calls(), kernel.value().expected_host_calls)
        << syscall_shape_name(shape);
    EXPECT_FALSE(host.faulted())
        << syscall_shape_name(shape)
        << ": an out-pointer write fell outside guest memory, so the host did "
           "less work than the shape claims";
    EXPECT_EQ(outcome.gas_consumed, kernel.value().scheduled_gas)
        << syscall_shape_name(shape)
        << " ran a different amount of work than the harness credits it with";
  }
}

// A selector wider than addi's signed 12-bit immediate encodes as a negative
// number, the host never matches it, and the kernel runs off the end of its
// code -- with the call count still looking right, because the terminal call
// was made. static_asserts in the header pin the constants; this pins the
// behaviour they exist to protect, so the run is known to end at the ecall
// rather than at a trap that happens to stop it.
TEST(Rv32GasCalibrationSyscall, TerminalCallHaltsRatherThanTrapping)
{
  SyscallKernelSpec spec;
  spec.shape = SyscallShape::kSeam;
  spec.body_slots = kSyscallTestBodySlots;
  spec.call_groups = 4;
  spec.trip_count = kSyscallTestTripCount;
  const auto kernel = build_syscall_kernel(spec);
  ASSERT_TRUE(kernel.has_value());

  auto program =
      ThreadedMachinePreInstantiation::create(kernel.value().code,
                                              kernel.value().initial_memory, 0,
                                              kernel.value().memory_bytes,
                                              /*trace=*/false);
  ASSERT_TRUE(program.has_value());

  ThreadedMachine machine(*program, kGasLimit);
  SyscallDispatchHost host(SyscallShape::kSeam);
  const RunOutcome outcome = run_once(machine, host);
  EXPECT_EQ(outcome.result, StepResult::kHalted);
  EXPECT_EQ(outcome.trap, Trap::kNone);
}

// Each ecall is a run barrier, so a body of N call groups must lower to N + 1
// straight-line runs. That is the structural statement behind the per-call
// cost: what is being priced is a crossing, and the crossing is what ends a
// run. A body that lowered to fewer runs than calls would mean some slot the
// harness counts as a call is not one.
TEST(Rv32GasCalibrationSyscall, EachCallIsItsOwnRunAndNothingFuses)
{
  for (const SyscallShape shape : kAllSyscallShapes) {
    for (const uint32_t groups :
         {0U, 5U, max_call_groups(kSyscallTestBodySlots, shape)}) {
      SyscallKernelSpec spec;
      spec.shape = shape;
      spec.body_slots = kSyscallTestBodySlots;
      spec.call_groups = groups;
      spec.trip_count = kSyscallTestTripCount;
      const auto kernel = build_syscall_kernel(spec);
      ASSERT_TRUE(kernel.has_value()) << syscall_shape_name(shape);

      const LoweringAudit audit =
          audit_body(kernel.value().code, kernel.value().initial_memory,
                     kernel.value().memory_bytes,
                     kernel.value().body_start_pc / kInstructionBytes,
                     spec.body_slots);
      EXPECT_TRUE(audit.clean())
          << syscall_shape_name(shape) << " at " << groups
          << " calls: lowering rewrote " << audit.rewritten_slots
          << " slot(s), first at " << audit.first_rewritten_slot;
      EXPECT_EQ(audit.body_runs, size_t{groups} + 1)
          << syscall_shape_name(shape) << " at " << groups << " calls";
    }
  }
}

// The seam kernels cannot borrow the opcode kernels' register plan: the syscall
// ABI is a0-a7 = x10-x17 and the opcode filler rotates over x14-x17, so an
// opcode filler between two calls would rewrite a4-a7 and the arguments the
// host sees would depend on how many filler slots preceded the call. Pin the
// disjointness rather than leave it to a comment.
TEST(Rv32GasCalibrationSyscall, FillerNeverWritesAnArgumentRegister)
{
  for (uint8_t reg = kSyscallFillerDstBase;
       reg < kSyscallFillerDstBase + kSyscallFillerDstSize; ++reg) {
    EXPECT_TRUE(reg < kSyscallArgBase ||
                reg >= kSyscallArgBase + kSyscallArgCount)
        << "filler destination x" << static_cast<int>(reg)
        << " aliases a syscall argument register";
  }
  EXPECT_TRUE(kSyscallLoopCounterReg < kSyscallArgBase ||
              kSyscallLoopCounterReg >= kSyscallArgBase + kSyscallArgCount);
  EXPECT_TRUE(kSyscallFillerSrcA < kSyscallArgBase ||
              kSyscallFillerSrcA >= kSyscallArgBase + kSyscallArgCount);
  EXPECT_TRUE(kSyscallFillerSrcB < kSyscallArgBase ||
              kSyscallFillerSrcB >= kSyscallArgBase + kSyscallArgCount);

  // And the emitted filler really does write where the plan says.
  SyscallKernelSpec spec;
  spec.shape = SyscallShape::kSelectorReload;
  spec.body_slots = kSyscallTestBodySlots;
  spec.call_groups = 0; // all filler
  spec.trip_count = kSyscallTestTripCount;
  const auto kernel = build_syscall_kernel(spec);
  ASSERT_TRUE(kernel.has_value());
  const auto program = ThreadedProgram::create(kernel.value().code);
  ASSERT_TRUE(program.has_value());
  const size_t body_start = kernel.value().body_start_pc / kInstructionBytes;
  for (size_t slot = 0; slot < spec.body_slots; ++slot) {
    const auto decoded = program->instruction_at(body_start + slot);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->op, Op::kAdd);
    EXPECT_TRUE(decoded->rd < kSyscallArgBase ||
                decoded->rd >= kSyscallArgBase + kSyscallArgCount)
        << "filler at slot " << slot << " writes x"
        << static_cast<int>(decoded->rd);
  }
}

// Out-pointer shapes are only measuring host work if the write lands. A write
// that faults is cheaper than one that does not and would be reported as a
// fast call, so the addresses the prologue loads have to be inside guest
// memory with room for the payload.
TEST(Rv32GasCalibrationSyscall, OutPointersAddressRealGuestMemory)
{
  EXPECT_LE(kSyscallOutPointerA + 32U, kDefaultMaxMemoryBytes);
  EXPECT_LE(kSyscallOutPointerB + 32U, kDefaultMaxMemoryBytes);
  EXPECT_NE(kSyscallOutPointerA, kSyscallOutPointerB);

  for (const SyscallShape shape : kAllSyscallShapes) {
    if (syscall_shape_output_bytes(shape) == 0) {
      continue;
    }
    SyscallKernelSpec spec;
    spec.shape = shape;
    spec.body_slots = kSyscallTestBodySlots;
    spec.saturated = true;
    spec.trip_count = kSyscallTestTripCount;
    const auto kernel = build_syscall_kernel(spec);
    ASSERT_TRUE(kernel.has_value()) << syscall_shape_name(shape);
    auto program =
        ThreadedMachinePreInstantiation::create(kernel.value().code,
                                                kernel.value().initial_memory,
                                                0, kernel.value().memory_bytes,
                                                /*trace=*/false);
    ASSERT_TRUE(program.has_value()) << syscall_shape_name(shape);
    ThreadedMachine machine(*program, kGasLimit);
    SyscallDispatchHost host(shape);
    const RunOutcome outcome = run_once(machine, host);
    ASSERT_EQ(outcome.result, StepResult::kHalted) << syscall_shape_name(shape);
    EXPECT_FALSE(host.faulted()) << syscall_shape_name(shape);
    // The host writes a payload derived from its call counter, so guest memory
    // at the out pointer must have changed from the zero image it started at.
    EXPECT_NE(machine.memory()[kSyscallOutPointerA], 0)
        << syscall_shape_name(shape) << " wrote nothing through a1";
  }
}

// A request for more call groups than the body can hold is refused rather than
// quietly truncated. Truncation would place fewer calls than the denominator
// says, which is the one failure mode a timing cannot reveal.
TEST(Rv32GasCalibrationSyscall, RefusesMoreCallsThanTheBodyHolds)
{
  for (const SyscallShape shape : kAllSyscallShapes) {
    SyscallKernelSpec spec;
    spec.shape = shape;
    spec.body_slots = kSyscallTestBodySlots;
    spec.call_groups = max_call_groups(kSyscallTestBodySlots, shape) + 1;
    spec.trip_count = kSyscallTestTripCount;
    const auto kernel = build_syscall_kernel(spec);
    ASSERT_FALSE(kernel.has_value()) << syscall_shape_name(shape);
    EXPECT_EQ(kernel.error(), KernelError::kTooManyTargets)
        << syscall_shape_name(shape);
  }
}

TEST(Rv32GasCalibrationFit, RecoversAKnownSlope)
{
  std::vector<MeasurementPoint> points;
  for (uint32_t x = 0; x <= 100; x += 25) {
    MeasurementPoint point;
    point.x = static_cast<double>(x);
    point.trip_count = 1;
    point.samples = {10.0 + (0.25 * x)};
    points.push_back(std::move(point));
  }
  const LinearFit fit = fit_line(points);
  EXPECT_NEAR(fit.slope, 0.25, 1e-9);
  EXPECT_NEAR(fit.intercept, 10.0, 1e-9);
  EXPECT_NEAR(fit.theil_sen_slope, 0.25, 1e-9);
  EXPECT_NEAR(fit.r_squared, 1.0, 1e-9);
  // A perfect fit has no residual, so it claims no uncertainty.
  EXPECT_NEAR(fit.slope_stderr, 0.0, 1e-9);
}

TEST(Rv32GasCalibrationFit, ReportsUncertaintyWhenPointsScatter)
{
  std::vector<MeasurementPoint> clean;
  std::vector<MeasurementPoint> scattered;
  for (uint32_t x = 0; x <= 100; x += 20) {
    MeasurementPoint point;
    point.x = static_cast<double>(x);
    point.trip_count = 1;
    point.samples = {10.0 + (0.25 * x)};
    clean.push_back(point);
    // One displaced point: OLS absorbs it into the slope, Theil-Sen does not,
    // and the standard error has to grow.
    if (x == 40) {
      point.samples = {10.0 + (0.25 * x) + 8.0};
    }
    scattered.push_back(std::move(point));
  }
  const LinearFit clean_fit = fit_line(clean);
  const LinearFit scattered_fit = fit_line(scattered);
  EXPECT_GT(scattered_fit.slope_stderr, clean_fit.slope_stderr);
  EXPECT_GT(scattered_fit.slope_stderr, 0.0);
  EXPECT_NE(scattered_fit.slope, scattered_fit.theil_sen_slope);
  EXPECT_NEAR(scattered_fit.theil_sen_slope, 0.25, 0.15);
}

TEST(Rv32GasCalibrationFit, MedianHandlesEvenAndOddCounts)
{
  EXPECT_NEAR(median_of({3.0, 1.0, 2.0}), 2.0, 1e-9);
  EXPECT_NEAR(median_of({4.0, 1.0, 3.0, 2.0}), 2.5, 1e-9);
  EXPECT_NEAR(median_of({}), 0.0, 1e-9);
}

TEST(Rv32GasCalibrationKernel, RejectsSpecsItCannotRepresent)
{
  KernelSpec spec;
  spec.target = Op::kEbreak; // terminates instead of executing the whole body
  spec.body_slots = kTestBodySlots;
  spec.trip_count = kTestTripCount;
  EXPECT_FALSE(build_kernel(spec).has_value());

  // More targets than the interior can hold without making two adjacent.
  spec.target = Op::kAdd;
  spec.target_slots = kTestBodySlots;
  EXPECT_FALSE(build_kernel(spec).has_value());
}

TEST(Rv32GasCalibrationKernel, ScheduledGasMatchesTheProductionDecoder)
{
  KernelSpec spec;
  spec.target = Op::kDiv;
  spec.body_slots = kTestBodySlots;
  spec.target_slots = kTestTargets;
  spec.trip_count = 1;
  const auto kernel = build_kernel(spec);
  ASSERT_TRUE(kernel.has_value());
  const auto decoded = scheduled_gas_for_code(kernel.value().code);
  ASSERT_TRUE(decoded.has_value());
  // One trip runs every instruction except the JAL back to the body head: the
  // loop exits by taking the skip branch, which jumps over it. So the
  // schedule's static total over the code is the run's charge plus that one
  // JAL. (Before the loop tail became beq-over-jal this was a plain equality,
  // because every instruction in the program executed.)
  EXPECT_EQ(decoded.value(),
            kernel.value().scheduled_gas + scheduled_gas_for(Op::kJal));
}

} // namespace
} // namespace hogd::rv32::gas_calibration
