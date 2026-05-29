#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "syscall_error.hh"

// Test host functions mirror a C ABI with many intentionally-unused slots.
// NOLINTBEGIN(readability-named-parameter)

namespace hogd::rv32 {

using std::int32_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

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
  return (static_cast<uint32_t>(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
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

// Build EcallArgs (a0..a7) from up to eight register values. The bridge
// sign-extends each, so callers pass the raw 32-bit register contents.
EcallArgs
make_ecall_args(std::initializer_list<uint64_t> regs)
{
  EcallArgs args = {};
  uint32_t *slots[] = {&args.a0, &args.a1, &args.a2, &args.a3,
                       &args.a4, &args.a5, &args.a6, &args.a7};
  size_t index = 0;
  for (const uint64_t reg : regs) {
    if (index < 8) {
      *slots[index] = static_cast<uint32_t>(reg);
    }
    ++index;
  }
  return args;
}

struct CapturedCall {
  std::array<uint64_t, 8> args = {};
  uint64_t gas_remaining = 0;
  uint64_t memory_size = 0;
  uint8_t first_memory_byte = 0;
  int calls = 0;
  TrampolineResult result = {.result_code = 0, .panic = 0, .gas_consumed = 0};
};

TrampolineResult
capture_entry(ExternHostCallContext context, WasmContext wasm_context,
              uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
              uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7)
{
  auto *captured = static_cast<CapturedCall *>(context);
  ++captured->calls;
  captured->args = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7};
  captured->gas_remaining = wasm_context.gas_remaining;
  captured->memory_size = wasm_context.memory.sz;
  captured->first_memory_byte = wasm_context.memory.mem[0];
  wasm_context.memory.mem[7] = 0xa5;
  return captured->result;
}

struct SequencedGasCall {
  int calls = 0;
  uint64_t gas_consumed = 0;
};

TrampolineResult
sequenced_gas_entry(ExternHostCallContext context, WasmContext wasm_context,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t)
{
  auto *captured = static_cast<SequencedGasCall *>(context);
  ++captured->calls;
  wasm_context.memory.mem[8] = static_cast<uint8_t>(0x40 + captured->calls);
  return {.result_code = 222,
          .panic = std::to_underlying(HostFnErrorCode::NoneOrRecoverable),
          .gas_consumed = captured->gas_consumed};
}

TrampolineResult
throwing_entry(ExternHostCallContext, WasmContext, uint64_t, uint64_t, uint64_t,
               uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
  throw 1;
}

enum class ParserProbeMode : uint64_t {
  kSpans = 1,
  kBool = 2,
  kSignedI32 = 3,
  kOptionalU32 = 4,
  kUnsignedU32 = 5,
};

TrampolineResult
parser_probe_entry(ExternHostCallContext, WasmContext wasm_context,
                   uint64_t mode, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t, uint64_t, uint64_t)
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
  case ParserProbeMode::kUnsignedU32:
  {
    auto parsed = SyscallArgParams<uint32_t>::parse(guard, memory, arg1);
    return {.result_code =
                SyscallArgParams<uint32_t>::validate(parsed) ? 0 : -1,
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

// Drive parser_probe_entry through the bridge: mode in a0, probe args in
// a1..a4. The probe's return code is delivered in a0 (HostResult continue
// value); there is no result footer under the register-native ABI.
HostResult
call_parser_probe(std::vector<uint8_t> &memory,
                  std::initializer_list<uint64_t> args)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  SyscallBridgeHost bridge(nullptr, parser_probe_entry);
  MachineView view(0, 10, registers, memory);
  return bridge.ecall(view, make_ecall_args(args));
}

TrampolineResult
require_last_result(const SyscallBridgeHost &bridge)
{
  const auto result = bridge.last_result();
  EXPECT_TRUE(result.has_value());
  return result.value_or(TrampolineResult{});
}

constexpr uint64_t kSpansMode = static_cast<uint64_t>(ParserProbeMode::kSpans);
constexpr uint64_t kBoolMode = static_cast<uint64_t>(ParserProbeMode::kBool);
constexpr uint64_t kSignedI32Mode =
    static_cast<uint64_t>(ParserProbeMode::kSignedI32);
constexpr uint64_t kOptionalU32Mode =
    static_cast<uint64_t>(ParserProbeMode::kOptionalU32);
constexpr uint64_t kUnsignedU32Mode =
    static_cast<uint64_t>(ParserProbeMode::kUnsignedU32);

TEST(Rv32SyscallBridgeTest, marshalsRegistersToHostFn8)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(128);
  memory[0] = 0x5c;

  MachineView view(0, 77, registers, memory);
  CapturedCall captured;
  captured.result = {.result_code = -7, .panic = 0, .gas_consumed = 12};
  SyscallBridgeHost bridge(&captured, capture_entry);

  // a0..a7 carry the syscall number plus seven argument words. Distinct,
  // non-sequential per-register values pin the exact register->arg mapping (a
  // swap or off-by-one in the sign_extend_arg call list would be caught); the
  // final register has bit 31 set to exercise the bridge's sign-extension.
  const HostResult result =
      bridge.ecall(view, make_ecall_args({0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                          0x77, 0xFFFFFFFFU}));

  ASSERT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xfffffff9U);
  EXPECT_EQ(result.gas_consumed, 12U);
  EXPECT_EQ(captured.calls, 1);
  EXPECT_EQ(captured.args[0], 0x11ULL);
  EXPECT_EQ(captured.args[1], 0x22ULL);
  EXPECT_EQ(captured.args[2], 0x33ULL);
  EXPECT_EQ(captured.args[3], 0x44ULL);
  EXPECT_EQ(captured.args[4], 0x55ULL);
  EXPECT_EQ(captured.args[5], 0x66ULL);
  EXPECT_EQ(captured.args[6], 0x77ULL);
  EXPECT_EQ(captured.args[7], 0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(captured.gas_remaining, 77U);
  EXPECT_EQ(captured.memory_size, memory.size());
  EXPECT_EQ(captured.first_memory_byte, 0x5cU);
  EXPECT_EQ(memory[7], 0xa5U);
  const TrampolineResult last_result = require_last_result(bridge);
  EXPECT_EQ(last_result.gas_consumed, 12U);
}

TEST(Rv32SyscallBridgeTest, parserProbeAcceptsValidSpansAndZeroLengthEndPointer)
{
  std::vector<uint8_t> memory(160);
  constexpr uint32_t kInput = 120;
  constexpr uint32_t kOutput = 128;
  memory[kInput] = 0x44;

  HostResult result =
      call_parser_probe(memory, {kSpansMode, kInput, 1, kOutput, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0U);
  EXPECT_EQ(memory[kOutput], 0x44U);

  result = call_parser_probe(memory,
                             {kSpansMode, memory.size(), 0, memory.size(), 0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0U);
}

TEST(Rv32SyscallBridgeTest, parserProbeRejectsOobAndAliasedSpans)
{
  std::vector<uint8_t> memory(160);

  HostResult result = call_parser_probe(memory, {kSpansMode, 159, 2, 128, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);

  result = call_parser_probe(memory, {kSpansMode, 120, 1, 159, 2});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);

  result = call_parser_probe(memory, {kSpansMode, 120, 8, 124, 4});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);
}

// The ABI's safety argument rests on valid guest pointers/lengths having bit 31
// clear. A pointer or length WITH bit 31 set sign-extends to a huge 64-bit
// value, which the parser must reject (out of bounds) rather than silently wrap
// to a small in-range offset.
TEST(Rv32SyscallBridgeTest, parserProbeRejectsBit31PointerOrLength)
{
  std::vector<uint8_t> memory(160);

  // Input pointer with bit 31 set -> sign-extends to ~0xFFFFFFFF80000000 ->
  // OOB.
  HostResult result =
      call_parser_probe(memory, {kSpansMode, 0x80000000U, 1, 120, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);

  // Length with bit 31 set -> sign-extends huge -> OOB.
  result = call_parser_probe(memory, {kSpansMode, 120, 0x80000000U, 124, 1});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);
}

// Documents the register-native ABI limit (finding #2): because the bridge
// sign-extends, an unsigned 32-bit scalar with bit 31 set (>= 2^31) exceeds the
// uint32 range and is rejected. Register-passed unsigned scalars are therefore
// limited to < 2^31; wider unsigned values must travel by pointer.
TEST(Rv32SyscallBridgeTest, parserProbeUnsignedScalarBit31IsUnreachable)
{
  std::vector<uint8_t> memory(160);

  EXPECT_EQ(call_parser_probe(memory, {kUnsignedU32Mode, 0}).continue_value(),
            0U);
  EXPECT_EQ(call_parser_probe(memory, {kUnsignedU32Mode, 0x7FFFFFFFU})
                .continue_value(),
            0U);
  // bit 31 set: sign-extended past UINT32_MAX -> parser fault.
  EXPECT_EQ(call_parser_probe(memory, {kUnsignedU32Mode, 0x80000000U})
                .continue_value(),
            0xffffFFFFU);
  EXPECT_EQ(call_parser_probe(memory, {kUnsignedU32Mode, 0xFFFFFFFFU})
                .continue_value(),
            0xffffFFFFU);
}

TEST(Rv32SyscallBridgeTest, parserProbeCanonicalizesRegisterScalars)
{
  std::vector<uint8_t> memory(160);

  // bool accepts only 0/1; 2 is not a canonical bool encoding.
  EXPECT_EQ(call_parser_probe(memory, {kBoolMode, 1}).continue_value(), 0U);
  EXPECT_EQ(call_parser_probe(memory, {kBoolMode, 0}).continue_value(), 0U);
  EXPECT_EQ(call_parser_probe(memory, {kBoolMode, 2}).continue_value(),
            0xffffFFFFU);

  // The bridge sign-extends every register, so any 32-bit pattern is a
  // canonical signed encoding. Unlike the WASM i64 ABI (which can present
  // non-canonical words), 0xFFFFFFFF and 0x80000000 are accepted here as -1
  // and INT_MIN respectively.
  EXPECT_EQ(call_parser_probe(memory, {kSignedI32Mode, 0}).continue_value(),
            0U);
  EXPECT_EQ(
      call_parser_probe(memory, {kSignedI32Mode, 0x7FFFFFFFU}).continue_value(),
      0U);
  EXPECT_EQ(
      call_parser_probe(memory, {kSignedI32Mode, 0xFFFFFFFFU}).continue_value(),
      0U);
  EXPECT_EQ(
      call_parser_probe(memory, {kSignedI32Mode, 0x80000000U}).continue_value(),
      0U);
}

TEST(Rv32SyscallBridgeTest, parserProbeOptionalPointerSentinelFromAllOnesReg)
{
  std::vector<uint8_t> memory(180);
  constexpr uint32_t kValueAddress = 120;
  store_le64(memory, kValueAddress, 37);
  store_le64(memory, 0, 5);

  // 0xFFFFFFFF sign-extends to UINT64_MAX, the parser's optional-pointer null
  // sentinel, so the optional argument resolves to None (probe returns 1).
  HostResult result =
      call_parser_probe(memory, {kOptionalU32Mode,
                                 std::numeric_limits<uint64_t>::max()});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 1U);

  // Offset 0 is a valid (non-sentinel) pointer reading memory[0..3] == 5.
  result = call_parser_probe(memory, {kOptionalU32Mode, 0});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 5U);

  // Out-of-bounds optional pointer fails validation.
  result = call_parser_probe(memory, {kOptionalU32Mode, 179});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 0xffffFFFFU);

  // In-bounds optional pointer dereferences the stored value.
  result = call_parser_probe(memory, {kOptionalU32Mode, kValueAddress});
  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(), 37U);
}

TEST(Rv32SyscallBridgeTest, trapsWithoutCallingNullEntry)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64);
  SyscallBridgeHost bridge(nullptr, nullptr);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(result.trap_payload().kind, HostTrapKind::kFatal);
  EXPECT_FALSE(bridge.last_result().has_value());
}

TEST(Rv32SyscallBridgeTest, trapsIfHostEntryThrows)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64);
  SyscallBridgeHost bridge(nullptr, throwing_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(result.trap_payload().kind, HostTrapKind::kFatal);
  EXPECT_FALSE(bridge.last_result().has_value());
}

TEST(Rv32SyscallBridgeTest, hostOutOfGasBecomesOutOfGasWithoutWriteback)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64);
  memory[7] = 0x11;
  CapturedCall captured;
  captured.result = {.result_code = 3,
                     .panic = std::to_underlying(HostFnErrorCode::OutOfGas),
                     .gas_consumed = 4};
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{});

  EXPECT_EQ(result.status, HostStatus::kOutOfGas);
  EXPECT_EQ(result.gas_consumed, 4U);
  EXPECT_EQ(captured.calls, 1);
  const TrampolineResult last_result = require_last_result(bridge);
  EXPECT_EQ(last_result.panic, std::to_underlying(HostFnErrorCode::OutOfGas));
  EXPECT_EQ(memory[7], 0x11U);
}

TEST(Rv32SyscallBridgeTest, fatalHostPanicBecomesHostTrap)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64);
  memory[7] = 0x11;
  CapturedCall captured;
  captured.result = {.result_code = 3,
                     .panic =
                         std::to_underlying(HostFnErrorCode::Unrecoverable),
                     .gas_consumed = 4};
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  const HostResult result = bridge.ecall(view, EcallArgs{});

  EXPECT_EQ(result.status, HostStatus::kTrap);
  EXPECT_EQ(result.trap_payload().kind, HostTrapKind::kFatal);
  EXPECT_EQ(result.trap_payload().code,
            static_cast<uint32_t>(HostFnErrorCode::Unrecoverable));
  EXPECT_EQ(result.gas_consumed, 4U);
  EXPECT_EQ(captured.calls, 1);
  const TrampolineResult last_result = require_last_result(bridge);
  EXPECT_EQ(last_result.panic,
            std::to_underlying(HostFnErrorCode::Unrecoverable));
  EXPECT_EQ(memory[7], 0x11U);
}

TEST(Rv32SyscallBridgeTest, recoverableFaultReturnsCodeWithoutWriteback)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64, 0x55);
  CapturedCall captured;
  captured.result = {.result_code = std::to_underlying(
                         SyscallErrorCode::kSyscallErrorCodeFault),
                     .panic =
                         std::to_underlying(HostFnErrorCode::NoneOrRecoverable),
                     .gas_consumed = 4};
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);
  const std::vector<uint8_t> before = memory;

  const HostResult result = bridge.ecall(view, EcallArgs{});

  EXPECT_EQ(result.status, HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(),
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeFault));
  EXPECT_EQ(result.gas_consumed, 4U);
  EXPECT_EQ(captured.calls, 1);
  EXPECT_EQ(memory, before);
}

TEST(Rv32SyscallBridgeTest, mapsReturnAndExitPanicsToHalt)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> memory(64);
  CapturedCall captured;
  SyscallBridgeHost bridge(&captured, capture_entry);
  MachineView view(0, 10, registers, memory);

  // capture_entry writes 0xa5 to scratch byte 7; both halt paths must commit
  // that scratch back to guest memory (the guest-visible Return output buffer
  // is delivered the same way).
  captured.result = {.result_code = 3,
                     .panic = std::to_underlying(HostFnErrorCode::Return),
                     .gas_consumed = 4};
  HostResult result = bridge.ecall(view, EcallArgs{});
  EXPECT_EQ(result.status, HostStatus::kHalt);
  EXPECT_EQ(result.halt_code(), 0U);
  EXPECT_EQ(result.gas_consumed, 4U);
  EXPECT_EQ(memory[7], 0xa5U);

  memory[7] = 0;
  captured.result = {.result_code = 9,
                     .panic = std::to_underlying(HostFnErrorCode::Exit),
                     .gas_consumed = 4};
  result = bridge.ecall(view, EcallArgs{});
  EXPECT_EQ(result.status, HostStatus::kHalt);
  EXPECT_EQ(result.halt_code(), 9U);
  EXPECT_EQ(result.gas_consumed, 4U);
  EXPECT_EQ(memory[7], 0xa5U);
}

TEST(Rv32SyscallBridgeTest, machineCompletesEcallThroughBridge)
{
  const auto program = make_program({
      addi(10, 0, 1), // a0 = syscall number 1
      addi(11, 0, 2), // a1 = argument 2
      ecall(),
  });
  Machine machine(program, 160, 20);

  CapturedCall captured;
  captured.result = {.result_code = 1234, .panic = 0, .gas_consumed = 5};
  SyscallBridgeHost bridge(&captured, capture_entry);

  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.step(), StepResult::kEcall);
  const auto pending_ecall = machine.pending_ecall();
  ASSERT_TRUE(pending_ecall.has_value());
  const HostResult result =
      bridge.ecall(machine.view(), pending_ecall.value_or(EcallArgs{}));
  EXPECT_EQ(machine.complete_ecall(result), StepResult::kContinue);
  EXPECT_EQ(machine.reg(10), 1234U);
  EXPECT_EQ(captured.calls, 1);
  EXPECT_EQ(captured.args[0], 1U);
  EXPECT_EQ(captured.args[1], 2U);
  EXPECT_EQ(machine.memory()[7], 0xa5U);
  EXPECT_EQ(machine.gas_remaining(), 12U);
}

TEST(Rv32SyscallBridgeTest, sameInputsProduceSameBridgeResultAndMemory)
{
  std::array<uint32_t, kRegisterCount> registers = {};
  std::vector<uint8_t> first_memory(128, 0x33);
  std::vector<uint8_t> second_memory = first_memory;

  CapturedCall first_captured;
  first_captured.result = {.result_code = 17,
                           .panic = std::to_underlying(
                               HostFnErrorCode::NoneOrRecoverable),
                           .gas_consumed = 6};
  CapturedCall second_captured = first_captured;
  SyscallBridgeHost first_bridge(&first_captured, capture_entry);
  SyscallBridgeHost second_bridge(&second_captured, capture_entry);
  MachineView first_view(0, 44, registers, first_memory);
  MachineView second_view(0, 44, registers, second_memory);

  const EcallArgs args = make_ecall_args(
      {0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007});
  const HostResult first_result = first_bridge.ecall(first_view, args);
  const HostResult second_result = second_bridge.ecall(second_view, args);

  EXPECT_EQ(first_result.status, second_result.status);
  EXPECT_EQ(first_result.continue_value(), second_result.continue_value());
  EXPECT_EQ(first_result.gas_consumed, second_result.gas_consumed);
  EXPECT_EQ(first_memory, second_memory);
  const TrampolineResult first_last_result = require_last_result(first_bridge);
  const TrampolineResult second_last_result =
      require_last_result(second_bridge);
  EXPECT_EQ(first_last_result.result_code, second_last_result.result_code);
  EXPECT_EQ(first_last_result.panic, second_last_result.panic);
  EXPECT_EQ(first_last_result.gas_consumed, second_last_result.gas_consumed);
}

TEST(Rv32SyscallBridgeTest, hostGasIsCumulativeAndSecondEcallRollsBack)
{
  const auto program = make_program({
      addi(10, 0, 0),
      ecall(),
      addi(10, 0, 0),
      ecall(),
  });
  Machine machine(program, 64, 9);
  SequencedGasCall captured{.gas_consumed = 3};
  SyscallBridgeHost bridge(&captured, sequenced_gas_entry);

  EXPECT_EQ(machine.run(bridge), StepResult::kTrap);

  EXPECT_EQ(captured.calls, 2);
  EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
  EXPECT_EQ(machine.gas_remaining(), 2U);
  EXPECT_EQ(machine.reg(10), 0U);
  EXPECT_EQ(machine.memory()[8], 0x41U);
}

} // namespace

// NOLINTEND(readability-named-parameter)
} // namespace hogd::rv32
