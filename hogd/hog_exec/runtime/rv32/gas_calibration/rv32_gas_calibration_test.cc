#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
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
// A kernel must run all of its body, at every body size the harness allows.
//
// This is the invariant that a silently-truncated loop breaks, and it broke:
// the loop used to close with a single backwards branch over the body, and a
// B-type immediate reaches 4096 bytes -- exactly 1024 instructions. At 1024
// body slots the encoder truncated the offset instead of refusing, so the
// kernel jumped into the middle of itself. At 1024 and 1536 it trapped, which
// the harness caught. At 2048 it reported a *clean halt* having burned 2107 of
// the 16443 gas it was credited with, which the harness did not catch: every
// opcode in that run priced at exactly the baseline, a pointer-chased load
// included, and the sweep looked beautifully reproducible because it had
// stopped measuring.
//
// Comparing consumed gas against credited gas catches all of that in one
// assertion, so size the bodies past the old cliff and check it.
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
  const std::vector<uint8_t> memory(kMaxMemoryBytes, 0);
  const LoweringAudit audit = audit_body(code, memory, kMaxMemoryBytes,
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
  spec.target = Op::kJal; // needs a per-slot rewritten target
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
