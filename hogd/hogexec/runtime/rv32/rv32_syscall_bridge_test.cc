#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"

namespace hogd::rv32 {
namespace {

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

void
store_le64(std::vector<uint8_t> &memory, uint32_t address, uint64_t value)
{
  for (uint32_t i = 0; i < sizeof(uint64_t); ++i) {
    memory[address + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

int32_t
load_le32(const std::vector<uint8_t> &memory, uint32_t address)
{
  uint32_t value = 0;
  for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
    value |= uint32_t{memory[address + i]} << (i * 8);
  }
  return static_cast<int32_t>(value);
}

std::shared_ptr<const Program>
make_program(std::initializer_list<uint32_t> words)
{
  std::vector<uint8_t> code;
  code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  auto result =
      Program::create(std::span<const uint8_t>(code.data(), code.size()));
  if (!result.has_value()) {
    ADD_FAILURE() << "failed to create test program";
    return {};
  }
  return std::make_shared<Program>(std::move(*result));
}

uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return (uint32_t(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | opcode;
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13, rd, 0, rs1, imm);
}

uint32_t
ecall()
{
  return 0x00000073U;
}

struct CapturedCall {
  std::array<uint64_t, kSyscallFrameArgWords> args = {};
  uint64_t gas_remaining = 0;
  uint64_t memory_size = 0;
  uint8_t first_memory_byte = 0;
  int calls = 0;
  TrampolineResult result = {.result_code = 0, .panic = 0, .gas_consumed = 0};
};

TrampolineResult
capture_entry(ExternHostCallContext context, WasmContext wasm_context,
              uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
              uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7,
              uint64_t arg8, uint64_t arg9, uint64_t arg10)
{
  auto *captured = static_cast<CapturedCall *>(context);
  ++captured->calls;
  captured->args = {arg0, arg1, arg2, arg3, arg4, arg5,
                    arg6, arg7, arg8, arg9, arg10};
  captured->gas_remaining = wasm_context.gas_remaining;
  captured->memory_size = wasm_context.memory.sz;
  captured->first_memory_byte = wasm_context.memory.mem[0];
  wasm_context.memory.mem[7] = 0xa5;
  return captured->result;
}

TrampolineResult
throwing_entry(ExternHostCallContext, WasmContext, uint64_t, uint64_t, uint64_t,
               uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
               uint64_t, uint64_t)
{
  throw 1;
}

enum class ParserProbeMode : uint64_t {
  kSpans = 1,
  kBool = 2,
  kSignedI32 = 3,
  kOptionalU32 = 4,
};

TrampolineResult
parser_probe_entry(ExternHostCallContext, WasmContext wasm_context,
                   uint64_t mode, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t, uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t)
{
  AliasGuard guard;
  std::span<std::byte> memory(reinterpret_cast<std::byte *>(
                                  wasm_context.memory.mem),
                              wasm_context.memory.sz);

  switch (static_cast<ParserProbeMode>(mode)) {
  case ParserProbeMode::kSpans:
  {
    auto input =
        SyscallArgParams<std::span<const std::byte>>::parse(guard, memory, arg1,
                                                            arg2);
    auto output = SyscallArgParams<std::span<std::byte>>::parse(guard, memory,
                                                                arg3, arg4);
    if (!SyscallArgParams<std::span<const std::byte>>::validate(input) ||
        !SyscallArgParams<std::span<std::byte>>::validate(output) ||
        !guard.check()) {
      return {.result_code = -1, .panic = 0, .gas_consumed = 0};
    }
    auto in = SyscallArgParams<std::span<const std::byte>>::deref(input);
    auto out = SyscallArgParams<std::span<std::byte>>::deref(output);
    if (!in.empty() && !out.empty()) {
      out[0] = in[0];
    }
    return {.result_code = 0, .panic = 0, .gas_consumed = 0};
  }
  case ParserProbeMode::kBool:
  {
    auto parsed = SyscallArgParams<bool>::parse(guard, memory, arg1);
    return {.result_code = SyscallArgParams<bool>::validate(parsed) ? 0 : -1,
            .panic = 0,
            .gas_consumed = 0};
  }
  case ParserProbeMode::kSignedI32:
  {
    auto parsed = SyscallArgParams<int32_t>::parse(guard, memory, arg1);
    return {.result_code = SyscallArgParams<int32_t>::validate(parsed) ? 0 : -1,
            .panic = 0,
            .gas_consumed = 0};
  }
  case ParserProbeMode::kOptionalU32:
  {
    auto parsed =
        SyscallArgParams<const uint32_t *>::parse(guard, memory, arg1);
    if (!SyscallArgParams<const uint32_t *>::validate(parsed)) {
      return {.result_code = -1, .panic = 0, .gas_consumed = 0};
    }
    const uint32_t *value = SyscallArgParams<const uint32_t *>::deref(parsed);
    return {.result_code = value == nullptr ? 1 : static_cast<int32_t>(*value),
            .panic = 0,
            .gas_consumed = 0};
  }
  }
  return {.result_code = -1, .panic = 0, .gas_consumed = 0};
}

void
write_bridge_frame(std::vector<uint8_t> &memory, uint32_t frame_address,
                   std::initializer_list<uint64_t> words)
{
  size_t index = 0;
  for (const uint64_t word : words) {
    store_le64(memory, frame_address + static_cast<uint32_t>(index * 8), word);
    ++index;
  }
}

HostResult
call_parser_probe(std::vector<uint8_t> &memory, uint32_t frame_address,
                  std::initializer_list<uint64_t> words)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  write_bridge_frame(memory, frame_address, words);
  SyscallBridgeHost bridge(nullptr, parser_probe_entry);
  MachineView view(0, 10, registers, memory);
  return bridge.ecall(view, EcallArgs{.a0 = frame_address, .a7 = 0});
}

TEST(Rv32SyscallBridgeTest, marshalsElevenWordFrameToHostFn11)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(128);
  memory[0] = 0x5c;
  constexpr uint32_t kFrameAddress = 16;
  for (size_t i = 0; i < kSyscallFrameArgWords; ++i) {
    store_le64(memory,
               kFrameAddress + static_cast<uint32_t>(i * sizeof(uint64_t)),
               0x0102030405060708ULL + i);
  }

  MachineView view(0, 77, registers, memory);
  CapturedCall captured;
  captured.result = {.result_code = -7, .panic = 0, .gas_consumed = 12};
  SyscallBridgeHost bridge(&captured, capture_entry);

  const HostResult result =
      bridge.ecall(view, EcallArgs{.a0 = kFrameAddress, .a7 = 0});

  ASSERT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xfffffff9U);
  EXPECT_EQ(captured.calls, 1);
  EXPECT_EQ(captured.args[0], 0x0102030405060708ULL);
  EXPECT_EQ(captured.args[10], 0x0102030405060712ULL);
  EXPECT_EQ(captured.gas_remaining, 77U);
  EXPECT_EQ(captured.memory_size, memory.size());
  EXPECT_EQ(captured.first_memory_byte, 0x5cU);
  EXPECT_EQ(memory[7], 0xa5U);
  ASSERT_TRUE(bridge.last_result().has_value());
  EXPECT_EQ(bridge.last_result()->gas_consumed, 12U);
  EXPECT_EQ(memory[kFrameAddress + kSyscallFrameResultOffset], 0xf9U);
  EXPECT_EQ(memory[kFrameAddress + kSyscallFrameResultOffset + 4], 0U);
  EXPECT_EQ(memory[kFrameAddress + kSyscallFrameResultOffset + 8], 12U);
}

TEST(Rv32SyscallBridgeTest, parserProbeAcceptsValidSpansAndZeroLengthEndPointer)
{
  std::vector<uint8_t> memory(160);
  constexpr uint32_t kFrameAddress = 0;
  constexpr uint32_t kInput = 120;
  constexpr uint32_t kOutput = 128;
  memory[kInput] = 0x44;

  HostResult result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSpans), kInput,
                         1, kOutput, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);
  EXPECT_EQ(memory[kOutput], 0x44U);
  EXPECT_EQ(load_le32(memory, kFrameAddress + kSyscallFrameResultOffset), 0);

  result = call_parser_probe(memory, kFrameAddress,
                             {static_cast<uint64_t>(ParserProbeMode::kSpans),
                              memory.size(), 0, memory.size(), 0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);
}

TEST(Rv32SyscallBridgeTest, parserProbeRejectsOobAndAliasedSpans)
{
  std::vector<uint8_t> memory(160);
  constexpr uint32_t kFrameAddress = 0;

  HostResult result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSpans), 159, 2,
                         128, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);
  EXPECT_EQ(load_le32(memory, kFrameAddress + kSyscallFrameResultOffset), -1);

  result = call_parser_probe(memory, kFrameAddress,
                             {static_cast<uint64_t>(ParserProbeMode::kSpans),
                              120, 1, 159, 2});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);
  EXPECT_EQ(load_le32(memory, kFrameAddress + kSyscallFrameResultOffset), -1);

  result = call_parser_probe(memory, kFrameAddress,
                             {static_cast<uint64_t>(ParserProbeMode::kSpans),
                              120, 8, 124, 4});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);
  EXPECT_EQ(load_le32(memory, kFrameAddress + kSyscallFrameResultOffset), -1);
}

TEST(Rv32SyscallBridgeTest, parserProbeEnforcesCanonicalScalars)
{
  std::vector<uint8_t> memory(160);
  constexpr uint32_t kFrameAddress = 0;

  HostResult result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kBool), 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kBool), 0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kBool), 2});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSignedI32),
                         0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSignedI32),
                         std::numeric_limits<int32_t>::max()});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSignedI32),
                         std::numeric_limits<uint64_t>::max()});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSignedI32),
                         0xffffFFFFU});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kSignedI32),
                         0x0000000080000000ULL});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);
}

TEST(Rv32SyscallBridgeTest, parserProbeUsesUint64MaxOptionalPointerSentinel)
{
  std::vector<uint8_t> memory(180);
  constexpr uint32_t kFrameAddress = 0;
  constexpr uint32_t kValueAddress = 120;
  store_le64(memory, kValueAddress, 37);

  HostResult result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kOptionalU32),
                         std::numeric_limits<uint64_t>::max()});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 1U);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kOptionalU32),
                         0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, static_cast<uint32_t>(ParserProbeMode::kOptionalU32));

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kOptionalU32),
                         179});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 0xffffFFFFU);

  result =
      call_parser_probe(memory, kFrameAddress,
                        {static_cast<uint64_t>(ParserProbeMode::kOptionalU32),
                         kValueAddress});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.value, 37U);
}

TEST(Rv32SyscallBridgeTest, trapsWithoutCallingNullEntry)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes);
  SyscallBridgeHost bridge(nullptr, nullptr);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_FALSE(bridge.last_result().has_value());
}

TEST(Rv32SyscallBridgeTest, trapsIfHostEntryThrows)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes);
  SyscallBridgeHost bridge(nullptr, throwing_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_FALSE(bridge.last_result().has_value());
}

TEST(Rv32SyscallBridgeTest, trapsBeforeCallingHostForInvalidFrame)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes - 1);
  CapturedCall captured;
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(captured.calls, 0);
  EXPECT_FALSE(bridge.last_result().has_value());
}

TEST(Rv32SyscallBridgeTest, trapsBeforeCallingHostForMisalignedFrame)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes + 1);
  CapturedCall captured;
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 1, .a7 = 0});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(captured.calls, 0);
}

TEST(Rv32SyscallBridgeTest, trapsOnUnsupportedBridgeVersion)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes);
  CapturedCall captured;
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 1});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(captured.calls, 0);
}

TEST(Rv32SyscallBridgeTest, hostPanicBecomesHostTrap)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes);
  CapturedCall captured;
  captured.result = {.result_code = 3, .panic = 1, .gas_consumed = 4};
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(captured.calls, 1);
  ASSERT_TRUE(bridge.last_result().has_value());
  EXPECT_EQ(bridge.last_result()->panic, 1);
  EXPECT_EQ(memory[kSyscallFrameResultOffset + 4], 1U);
}

TEST(Rv32SyscallBridgeTest, mapsReturnAndExitPanicsToHalt)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(kSyscallFrameBytes);
  CapturedCall captured;
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  captured.result = {.result_code = 3, .panic = 3, .gas_consumed = 4};
  HostResult result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});
  EXPECT_EQ(result.status, HostStatus::kHalt);
  EXPECT_EQ(result.value, 0U);

  captured.result = {.result_code = 9, .panic = 4, .gas_consumed = 4};
  result = bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = 0});
  EXPECT_EQ(result.status, HostStatus::kHalt);
  EXPECT_EQ(result.value, 9U);
}

TEST(Rv32SyscallBridgeTest, machineCompletesEcallThroughBridge)
{
  const auto program = make_program({
      addi(10, 0, 32),
      addi(17, 0, kSyscallBridgeVersion),
      ecall(),
  });
  Machine machine(program, 160, 20);
  std::vector<uint8_t> frame(kSyscallFrameBytes);
  for (size_t i = 0; i < kSyscallFrameArgWords; ++i) {
    store_le64(frame, static_cast<uint32_t>(i * sizeof(uint64_t)), i + 1);
  }
  ASSERT_TRUE(machine.view().write_memory(32, frame));

  CapturedCall captured;
  captured.result = {.result_code = 1234, .panic = 0, .gas_consumed = 5};
  SyscallBridgeHost bridge(&captured, capture_entry);

  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.step(), StepResult::kEcall);
  ASSERT_TRUE(machine.pending_ecall().has_value());
  const HostResult result =
      bridge.ecall(machine.view(), *machine.pending_ecall());
  EXPECT_EQ(machine.complete_ecall(result), StepResult::kContinue);
  EXPECT_EQ(machine.reg(10), 1234U);
  EXPECT_EQ(captured.calls, 1);
  EXPECT_EQ(captured.args[0], 1U);
  EXPECT_EQ(machine.memory()[7], 0xa5U);
}

} // namespace
} // namespace hogd::rv32
