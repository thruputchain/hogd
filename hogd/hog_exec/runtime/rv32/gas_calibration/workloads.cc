#include "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/measure.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

namespace hogd::rv32::gas_calibration {
namespace {

constexpr uint32_t kWorkloadBodySlots = 128;

// Address that stays inside the low 4 KiB the prologue's L1 working set covers.
int32_t
walk_offset(uint32_t index)
{
  return static_cast<int32_t>((index * 64U) % 4096U) - 2048;
}

std::vector<uint32_t>
alu_mix_body()
{
  // Rotating R-type ALU ops on independent destinations. None of the ALU-ALU
  // fusion pairs (sll;or, addi;addi, addi;sll, srl;addi) appear, so this is
  // the unfused end of the range.
  std::vector<uint32_t> body;
  const std::array<std::pair<uint8_t, uint8_t>, 5> ops = {
      {{0, 0x00}, {4, 0x00}, {7, 0x00}, {6, 0x00}, {0, 0x20}}};
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    const auto [funct3, funct7] = ops[i % ops.size()];
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(
        test_program::op(rd, kSrcLargeReg, kSrcSmallReg, funct3, funct7));
  }
  return body;
}

std::vector<uint32_t>
address_math_body()
{
  // addi;addi pairs: the back-to-back address math the ALU-ALU rule targets.
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(test_program::addi(rd, kSrcLargeReg, 16));
    body.push_back(test_program::addi(rd, rd, 8));
  }
  return body;
}

std::vector<uint32_t>
word_copy_body()
{
  // lw;sw pairs: the memcpy shape fuse_load_store exists for.
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    body.push_back(
        test_program::load(kLoadDstBase, 2, kAddressBankBase, walk_offset(i)));
    body.push_back(test_program::store(kAddressBankBase, kLoadDstBase,
                                       walk_offset(i + 1), 2));
  }
  return body;
}

std::vector<uint32_t>
load_use_body()
{
  // lw then an ALU op consuming the loaded register: fuse_load_use.
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    body.push_back(
        test_program::load(kLoadDstBase, 2, kAddressBankBase, walk_offset(i)));
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(test_program::op(rd, kLoadDstBase, kSrcSmallReg, 0, 0x00));
  }
  return body;
}

std::vector<uint32_t>
branch_dense_body()
{
  // addi;bne(not taken) pairs: loop-induction shape, one run barrier every two
  // instructions. The engine's per-run entry cost is at its worst here.
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(test_program::addi(rd, rd, 1));
    body.push_back(test_program::branch(1, kSrcLargeReg, kSrcLargeReg, 8));
  }
  return body;
}

std::vector<uint32_t>
multiply_mix_body()
{
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(test_program::op(rd, kSrcLargeReg, kSrcSmallReg, 0, 0x01));
    body.push_back(test_program::op(rd, rd, kSrcSmallReg, 0, 0x00));
  }
  return body;
}

std::vector<uint32_t>
divide_mix_body()
{
  std::vector<uint32_t> body;
  for (uint32_t i = 0; body.size() < kWorkloadBodySlots; ++i) {
    const auto rd = static_cast<uint8_t>(kDataBankBase + (i % kDataBankSize));
    body.push_back(test_program::op(rd, kSrcLargeReg, kSrcSmallReg, 4, 0x01));
    body.push_back(test_program::op(rd, rd, kSrcSmallReg, 0, 0x00));
  }
  return body;
}

struct Workload {
  std::string name;
  std::string shape;
  std::vector<uint32_t> (*body)();
};

const std::vector<Workload> &
workload_table()
{
  static const std::vector<Workload> table = {
      {"alu_mix", "unfused ALU", &alu_mix_body},
      {"address_math", "addi;addi (fused)", &address_math_body},
      {"word_copy", "lw;sw (fused)", &word_copy_body},
      {"load_use", "lw;alu (fused)", &load_use_body},
      {"branch_dense", "addi;bne (fused, barrier-heavy)", &branch_dense_body},
      {"multiply_mix", "mul;add", &multiply_mix_body},
      {"divide_mix", "div;add", &divide_mix_body},
  };
  return table;
}

struct BuiltWorkload {
  std::vector<uint8_t> code;
  std::vector<uint8_t> memory;
  size_t body_start_index = 0;
  size_t body_slots = 0;
  uint64_t scheduled_gas_per_iteration = 0;
  uint64_t instructions_per_iteration = 0;
  std::map<std::string, uint64_t> op_counts;
};

std::optional<BuiltWorkload>
build_workload(const Workload &workload, uint32_t trip_count)
{
  std::vector<uint32_t> body = workload.body();
  body.resize(kWorkloadBodySlots);
  // Two trailing R-type ADDs keep any branch in the body clear of the loop
  // control and pin the body/loop boundary to a non-fusible pair.
  body.push_back(filler_instruction_word(0));
  body.push_back(filler_instruction_word(1));

  std::vector<uint32_t> words;
  emit_prologue(words, WorkingSet::kL1Region, trip_count);
  const size_t body_start_index = words.size();
  words.insert(words.end(), body.begin(), body.end());
  words.push_back(test_program::addi(kLoopCounterReg, kLoopCounterReg, -1));
  words.push_back(test_program::branch(
      1, kLoopCounterReg, 0,
      -static_cast<int32_t>((body.size() + 1) * kInstructionBytes)));
  words.push_back(0x00000073U);

  BuiltWorkload built;
  built.code = test_program::code_from_words(words);
  built.memory.assign(kMaxMemoryBytes, 0);
  built.body_start_index = body_start_index;
  built.body_slots = body.size();
  built.instructions_per_iteration = body.size() + 2;

  // Gas is charged per decoded instruction, before fusion, so the iteration's
  // charge is just the body's decoded gas plus the loop control's.
  const std::vector<uint8_t> body_code = test_program::code_from_words(body);
  const auto body_gas = scheduled_gas_for_code(body_code);
  if (!body_gas.has_value()) {
    return std::nullopt;
  }
  built.scheduled_gas_per_iteration =
      *body_gas + scheduled_gas_for(Op::kAddi) + scheduled_gas_for(Op::kBne);

  std::vector<uint32_t> iteration = body;
  iteration.push_back(test_program::addi(kLoopCounterReg, kLoopCounterReg, -1));
  iteration.push_back(test_program::branch(1, kLoopCounterReg, 0, -4));
  const std::vector<uint8_t> iteration_code =
      test_program::code_from_words(iteration);
  const auto decoded = ThreadedProgram::create(iteration_code);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < decoded->instruction_count(); ++i) {
    const auto instruction = decoded->instruction_at(i);
    if (!instruction.has_value()) {
      return std::nullopt;
    }
    ++built.op_counts[op_name(instruction->op)];
  }
  return built;
}

} // namespace

std::vector<WorkloadResult>
measure_workloads(uint32_t repeats, uint32_t warmup_runs, double target_ns)
{
  std::vector<WorkloadResult> results;
  std::vector<BuiltWorkload> built;
  std::vector<std::unique_ptr<ThreadedMachinePreInstantiation>> programs;
  std::vector<MeasurementPoint> points;

  // Tune each workload's trip count on its own, then measure them together in
  // one interleaved schedule so they share whatever drift the host has.
  std::vector<uint32_t> trip_counts;
  for (const Workload &workload : workload_table()) {
    const std::optional<BuiltWorkload> seed = build_workload(workload, 64);
    if (!seed.has_value()) {
      continue;
    }
    auto seed_program =
        ThreadedMachinePreInstantiation::create(seed->code, seed->memory, 0,
                                                kMaxMemoryBytes,
                                                /*trace=*/false);
    if (!seed_program.has_value()) {
      continue;
    }
    trip_counts.push_back(tune_trip_count(*seed_program, 64, target_ns));
  }

  size_t index = 0;
  for (const Workload &workload : workload_table()) {
    if (index >= trip_counts.size()) {
      break;
    }
    const std::optional<BuiltWorkload> tuned =
        build_workload(workload, trip_counts[index]);
    if (!tuned.has_value()) {
      ++index;
      continue;
    }
    auto program =
        ThreadedMachinePreInstantiation::create(tuned->code, tuned->memory, 0,
                                                kMaxMemoryBytes,
                                                /*trace=*/false);
    if (!program.has_value()) {
      ++index;
      continue;
    }
    built.push_back(*tuned);
    programs.push_back(
        std::make_unique<ThreadedMachinePreInstantiation>(std::move(*program)));
    WorkloadResult result;
    result.name = workload.name;
    result.shape = workload.shape;
    result.scheduled_gas_per_iteration = tuned->scheduled_gas_per_iteration;
    result.instructions_per_iteration = tuned->instructions_per_iteration;
    result.op_counts = tuned->op_counts;
    result.fused_slots = audit_body(tuned->code, tuned->memory, kMaxMemoryBytes,
                                    tuned->body_start_index, tuned->body_slots)
                             .rewritten_slots;
    results.push_back(std::move(result));
    ++index;
  }

  points.reserve(programs.size());
  for (size_t i = 0; i < programs.size(); ++i) {
    points.push_back({.label = results[i].name,
                      .x = static_cast<double>(i),
                      .program = programs[i].get(),
                      .trip_count = trip_counts[i]});
  }
  measure_interleaved(points, {.repeats = repeats, .warmup_runs = warmup_runs});

  for (size_t i = 0; i < results.size(); ++i) {
    results[i].ns_per_iteration = points[i].median();
    results[i].relative_mad = points[i].relative_mad();
    if (results[i].scheduled_gas_per_iteration != 0) {
      results[i].ns_per_gas =
          results[i].ns_per_iteration /
          static_cast<double>(results[i].scheduled_gas_per_iteration);
    }
  }
  return results;
}

} // namespace hogd::rv32::gas_calibration
