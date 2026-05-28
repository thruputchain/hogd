#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/no_syscall_test.h"
#include "built_contracts/read_only_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/compiled_sha256_return_fixture.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/runtime_context.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

// Test callback fixtures intentionally mirror C host function ABIs, and the
// test fixture combines gtest with the existing native-contract harness.
// NOLINTBEGIN(readability-named-parameter,misc-multiple-inheritance)

namespace hogd {
namespace {

namespace compiled_sha256_return = rv32::fixtures::compiled_sha256_return;

static_assert(compiled_sha256_return::kSha256RepeatedMethodId ==
              sha256_wrapper::Sha256Wrapper::kEntrypoints[1]);
static_assert(compiled_sha256_return::kSha256MethodId ==
              sha256_wrapper::Sha256Wrapper::kEntrypoints[0]);

constexpr int32_t kRv32ContractId = 7;
constexpr ContractAddress kWasmContractAddress =
    NativeContractTestFramework::make_contract_address(6);
constexpr ContractAddress kRv32ContractAddress =
    NativeContractTestFramework::make_contract_address(7);
constexpr uint32_t kFrameAddress = 64;
constexpr uint32_t kReturnBufferAddress = 192;
constexpr uint32_t kShaInputAddress = 224;
constexpr uint32_t kShaOutputAddress = 320;
constexpr uint32_t kShaMemoryBytes = 512;
constexpr uint32_t kEffectBytestringFrameAddress = 64;
constexpr uint32_t kEffectLogEventFrameAddress = 192;
constexpr uint32_t kEffectReturnFrameAddress = 320;
constexpr uint32_t kEffectStorageKeyAddress = 448;
constexpr uint32_t kEffectStorageValueAddress = 480;
constexpr uint32_t kEffectTopicAddress = 512;
constexpr uint32_t kEffectEventDataAddress = 544;
constexpr uint32_t kEffectMemoryBytes = 768;
constexpr GasAmount kEnoughGas = 1000;
constexpr GasAmount kParityGas = 10'000;

constexpr std::string_view kEffectStorageKey = "rv32_parity_key";
constexpr std::string_view kEffectStorageValue = "rv32 parity bytes";
constexpr std::string_view kEffectEventData = "rv32 parity event";
constexpr GasAmount kWasmErrorHostGas = 100;
constexpr std::array<WasmEngine, 2> kDifferentialWasmEngines = {
    WasmEngine::kWasmi,
    WasmEngine::kWasmtime,
};

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

std::shared_ptr<const rv32::Program>
make_program(std::initializer_list<uint32_t> words)
{
  std::vector<uint8_t> code;
  code.reserve(words.size() * rv32::kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  auto result =
      rv32::Program::create(std::span<const uint8_t>(code.data(), code.size()));
  if (!result.has_value()) {
    ADD_FAILURE() << "failed to create RV32 test program";
    return {};
  }
  return std::make_shared<rv32::Program>(std::move(*result));
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
s_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0xfffU;
  return (uimm >> 5) << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | (uimm & 0x1fU) << 7 | 0x23U;
}

uint32_t
sw(uint8_t rs2, uint8_t rs1, int32_t imm)
{
  return s_type(2, rs1, rs2, imm);
}

uint32_t
ecall()
{
  return 0x00000073U;
}

TrampolineResult
make_host_panic_result(HostFnErrorCode code, uint64_t gas_consumed = 4)
{
  return to_trampoline_result(HostFnError{code}, gas_consumed);
}

TrampolineResult
make_exit_result(uint32_t code, uint64_t gas_consumed = 4)
{
  return {.result_code = static_cast<int32_t>(code),
          .panic = std::to_underlying(HostFnErrorCode::Exit),
          .gas_consumed = gas_consumed};
}

extern "C" TrampolineResult
unrecoverable_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
  return make_host_panic_result(HostFnErrorCode::Unrecoverable);
}

extern "C" TrampolineResult
validation_failure_entry(ExternHostCallContext /*unused*/,
                         WasmContext /*unused*/, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
  return make_host_panic_result(HostFnErrorCode::ValidationFailure);
}

extern "C" TrampolineResult
abandon_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
  return make_host_panic_result(HostFnErrorCode::Abandon);
}

extern "C" TrampolineResult
unknown_panic_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
  return {.result_code = 0, .panic = 999, .gas_consumed = 4};
}

extern "C" TrampolineResult
throwing_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
               uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
               uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
  throw 1;
}

extern "C" TrampolineResult
host_out_of_gas_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint64_t)
{
  return make_host_panic_result(HostFnErrorCode::OutOfGas, 0);
}

extern "C" TrampolineResult
exit_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint64_t,
           uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
           uint64_t, uint64_t, uint64_t)
{
  return make_exit_result(17);
}

extern "C" TrampolineResult
return_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint64_t,
             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
             uint64_t, uint64_t, uint64_t, uint64_t)
{
  return make_host_panic_result(HostFnErrorCode::Return);
}

extern "C" TrampolineResult
wasm_unrecoverable_entry(ExternHostCallContext /*unused*/,
                         WasmContext /*unused*/)
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Unrecoverable},
                              kWasmErrorHostGas);
}

extern "C" TrampolineResult
wasm_validation_failure_entry(ExternHostCallContext /*unused*/,
                              WasmContext /*unused*/)
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::ValidationFailure},
                              kWasmErrorHostGas);
}

extern "C" TrampolineResult
wasm_abandon_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/)
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Abandon},
                              kWasmErrorHostGas);
}

extern "C" TrampolineResult
wasm_good_call_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/)
{
  return to_trampoline_result({SyscallErrorCode::kSyscallErrorCodeNone},
                              kWasmErrorHostGas);
}

extern "C" TrampolineResult
wasm_consume_too_much_gas_entry(ExternHostCallContext /*unused*/,
                                WasmContext wasm_context)
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::OutOfGas},
                              wasm_context.gas_remaining);
}

void
store_le64(std::span<uint8_t> bytes, uint32_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
write_return_frame(std::span<uint8_t> memory, std::span<const uint8_t> payload)
{
  const std::array<uint64_t, 6> frame_words = {
      static_cast<uint64_t>(SyscallNo::kReturn),
      kReturnBufferAddress,
      payload.size(),
      0,
      0,
      0,
  };
  for (size_t i = 0; i < frame_words.size(); ++i) {
    store_le64(memory, kFrameAddress + (i * sizeof(uint64_t)), frame_words[i]);
  }
  std::ranges::copy(payload, memory.begin() + kReturnBufferAddress);
}

void
write_syscall_frame(std::span<uint8_t> memory, uint32_t frame_address,
                    std::initializer_list<uint64_t> words)
{
  ASSERT_LE(words.size(), rv32::kSyscallFrameArgWords);
  size_t i = 0;
  for (const uint64_t word : words) {
    store_le64(memory, frame_address + (i * sizeof(uint64_t)), word);
    ++i;
  }
}

std::vector<uint8_t>
make_initial_return_memory(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  write_return_frame(initial_memory, payload);
  return initial_memory;
}

rv32::RuntimeContractImage
make_single_ecall_image(HostFn11 entry)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
      .entry = entry,
  };
}

rv32::RuntimeContractImage
make_bridge_abi_fault_image(uint32_t frame_address, uint32_t bridge_version)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, frame_address),
          addi(17, 0, bridge_version),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
      .entry = &unrecoverable_entry,
  };
}

rv32::RuntimeContractImage
make_return_image(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory = make_initial_return_memory(payload);
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_two_method_return_image(std::span<const uint8_t> zero_payload,
                             std::span<const uint8_t> one_payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  constexpr uint32_t kSecondFrameAddress = 128;
  constexpr uint32_t kZeroReturnBufferAddress = 192;
  constexpr uint32_t kOneReturnBufferAddress = 224;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  write_syscall_frame(initial_memory, kFrameAddress,
                      {static_cast<uint64_t>(SyscallNo::kReturn),
                       kZeroReturnBufferAddress, zero_payload.size(), 0, 0, 0});
  write_syscall_frame(initial_memory, kSecondFrameAddress,
                      {static_cast<uint64_t>(SyscallNo::kReturn),
                       kOneReturnBufferAddress, one_payload.size(), 0, 0, 0});
  std::ranges::copy(zero_payload,
                    initial_memory.begin() + kZeroReturnBufferAddress);
  std::ranges::copy(one_payload,
                    initial_memory.begin() + kOneReturnBufferAddress);

  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
          addi(10, 0, kSecondFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .dispatch_table = {{.method_id = 0, .pc = 0},
                         {.method_id = 1, .pc = 3 * rv32::kInstructionBytes}},
      .memory_bytes = kMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_sha256_then_return_image(std::span<const uint8_t> input)
{
  std::vector<uint8_t> initial_memory(kShaMemoryBytes);
  const std::array<uint64_t, 4> frame_words = {
      static_cast<uint64_t>(SyscallNo::kSHA256),
      kShaInputAddress,
      input.size(),
      kShaOutputAddress,
  };
  for (size_t i = 0; i < frame_words.size(); ++i) {
    store_le64(initial_memory, kFrameAddress + (i * sizeof(uint64_t)),
               frame_words[i]);
  }
  std::ranges::copy(input, initial_memory.begin() + kShaInputAddress);

  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(8, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
          addi(5, 0, std::to_underlying(SyscallNo::kReturn)),
          sw(5, 8, 0),
          addi(5, 0, kShaOutputAddress),
          sw(5, 8, 8),
          addi(5, 0, SHA256Hash{}.size()),
          sw(5, 8, 16),
          sw(0, 8, 24),
          addi(10, 8, 0),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kShaMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_compiled_sha256_then_return_image()
{
  auto artifact = rv32::parse_artifact(compiled_sha256_return::kArtifact);
  if (!artifact.has_value()) {
    ADD_FAILURE() << "failed to parse compiled RV32 artifact";
    return {};
  }
  return rv32::RuntimeContractImage{
      .program = std::move(artifact->program),
      .initial_memory = std::move(artifact->initial_memory),
      .dispatch_table = std::move(artifact->dispatch_table),
      .memory_bytes = artifact->memory_bytes,
  };
}

rv32::RuntimeContractImage
make_artifact_runtime_image(std::initializer_list<uint32_t> words,
                            std::vector<uint8_t> initial_memory,
                            std::vector<rv32::DispatchEntry> dispatch_table = {
                                {.method_id = 0, .pc = 0}})
{
  const std::vector<uint8_t> artifact =
      rv32::test_artifact::build_artifact(words, initial_memory,
                                          dispatch_table);
  auto parsed = rv32::parse_artifact(artifact);
  if (!parsed.has_value()) {
    ADD_FAILURE() << "failed to parse synthetic RV32 artifact";
    return {};
  }
  return rv32::RuntimeContractImage{
      .program = std::move(parsed->program),
      .initial_memory = std::move(parsed->initial_memory),
      .dispatch_table = std::move(parsed->dispatch_table),
      .memory_bytes = parsed->memory_bytes,
  };
}

rv32::RuntimeContractImage
make_artifact_arithmetic_control_flow_image()
{
  const std::array<uint8_t, 4> return_placeholder = {};
  std::vector<uint8_t> initial_memory =
      make_initial_return_memory(return_placeholder);

  return make_artifact_runtime_image(
      {
          addi(6, 0, 0),
          addi(5, 0, 6),
          addi(5, 5, -1),
          rv32::test_program::op(6, 6, 5, 0, 0),
          rv32::test_program::branch(1, 5, 0, -8),
          addi(8, 0, kReturnBufferAddress),
          sw(6, 8, 0),
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      },
      std::move(initial_memory));
}

rv32::RuntimeContractImage
make_artifact_memory_transform_image()
{
  constexpr uint32_t kSourceAddress = 224;
  constexpr uint32_t kCopyBytes = 16;
  const std::array<uint8_t, kCopyBytes> return_placeholder = {};
  std::vector<uint8_t> initial_memory =
      make_initial_return_memory(return_placeholder);
  for (uint32_t i = 0; i < kCopyBytes; ++i) {
    initial_memory[kSourceAddress + i] = static_cast<uint8_t>(0x30U + i);
  }

  return make_artifact_runtime_image(
      {
          addi(1, 0, kSourceAddress),
          addi(2, 0, kReturnBufferAddress),
          addi(3, 0, kCopyBytes),
          i_type(0x03, 4, 0, 1, 0),
          addi(4, 4, 1),
          rv32::test_program::store(2, 4, 0, 0),
          addi(1, 1, 1),
          addi(2, 2, 1),
          addi(3, 3, -1),
          rv32::test_program::branch(1, 3, 0, -24),
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      },
      std::move(initial_memory));
}

rv32::RuntimeContractImage
make_storage_event_return_image()
{
  std::vector<uint8_t> initial_memory(kEffectMemoryBytes);
  const StorageKeyContractData key = NativeContractTestFramework::
      make_storage_key_matching_static_key_sdk_macro(kEffectStorageKey);
  std::ranges::copy(key, initial_memory.begin() + kEffectStorageKeyAddress);
  std::ranges::copy(kEffectStorageValue,
                    initial_memory.begin() + kEffectStorageValueAddress);
  std::fill_n(initial_memory.begin() + kEffectTopicAddress, SHA256Hash{}.size(),
              0x5aU);
  std::ranges::copy(kEffectEventData,
                    initial_memory.begin() + kEffectEventDataAddress);

  write_syscall_frame(initial_memory, kEffectBytestringFrameAddress,
                      {static_cast<uint64_t>(SyscallNo::kBytestringSet),
                       kEffectStorageKeyAddress, kEffectStorageValueAddress,
                       kEffectStorageValue.size(),
                       std::to_underlying(StorageDeltaFlags::None)});
  write_syscall_frame(initial_memory, kEffectLogEventFrameAddress,
                      {static_cast<uint64_t>(SyscallNo::kLogEvent),
                       kEffectTopicAddress,
                       std::numeric_limits<uint64_t>::max(),
                       std::numeric_limits<uint64_t>::max(),
                       kEffectEventDataAddress, kEffectEventData.size()});
  write_syscall_frame(initial_memory, kEffectReturnFrameAddress,
                      {static_cast<uint64_t>(SyscallNo::kReturn), 0, 0, 0, 0,
                       0});

  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(17, 0, rv32::kSyscallBridgeVersion),
          addi(10, 0, kEffectBytestringFrameAddress),
          ecall(),
          addi(10, 0, kEffectLogEventFrameAddress),
          ecall(),
          addi(10, 0, kEffectReturnFrameAddress),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kEffectMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_bad_pc_image()
{
  return rv32::RuntimeContractImage{
      .program = make_program({addi(1, 0, 1)}),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_bad_memory_image()
{
  return rv32::RuntimeContractImage{
      .program = make_program({
          i_type(0x03, 1, 2, 0, 254),
      }),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_misaligned_access_image()
{
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(1, 0, 1),
          i_type(0x03, 2, 2, 1, 0),
      }),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_invalid_syscall_then_return_image()
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  store_le64(initial_memory, kFrameAddress,
             static_cast<uint64_t>(kSyscallCount));
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(8, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
          addi(5, 0, std::to_underlying(SyscallNo::kReturn)),
          sw(5, 8, 0),
          addi(10, 8, 0),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
  };
}

InvokeResult
direct_invoke(std::shared_ptr<const Contract> const &contract,
              rv32::RuntimeContext &rv32_context, SystemCallHandler &handler,
              GasAmount gas_limit, Runtime::MethodID method_id = 0)
{
  const DeployedContractHash deployed =
      NativeContractTestFramework::make_deployed_contract_hash(contract);
  auto runtime = rv32_context.make_new_runtime(deployed, &handler);
  EXPECT_TRUE(runtime.has_value());
  EXPECT_NE(runtime.value(), nullptr);
  if (!runtime.has_value() || runtime.value() == nullptr) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::Unrecoverable},
                        .gas_consumed_ = 0};
  }
  return runtime.value()->invoke(method_id, gas_limit);
}

InvokeResult
invoke_wasm_error_contract(WasmEngine engine, HostFn0 external_call,
                           std::string_view method, GasAmount gas_limit)
{
  WasmTestFramework wasm;
  Contract contract;
  contract.contract_type(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_error_handling.wasm");
  const DeployedContractHash deployed{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  wasm.setup_wasm_context(engine);
  EXPECT_TRUE(wasm.link_fn("test", "good_call", &wasm_good_call_entry));
  EXPECT_TRUE(wasm.link_fn("test", "external_call", external_call));
  wasm.make_syscall_handler(contract);
  auto runtime =
      wasm.get_context()->make_new_runtime(deployed,
                                           wasm.get_syscall_handler());
  EXPECT_TRUE(runtime.has_value());
  if (!runtime.has_value()) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::Unrecoverable},
                        .gas_consumed_ = 0};
  }
  EXPECT_NE(runtime.value(), nullptr);
  if (runtime.value() == nullptr) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::Unrecoverable},
                        .gas_consumed_ = 0};
  }
  return runtime.value()->invoke(method, gas_limit);
}

void
expect_matching_error(InvokeResult const &wasm_result,
                      InvokeResult const &rv32_result, InvokeErrorCode expected)
{
  ASSERT_FALSE(wasm_result.is_root_success());
  ASSERT_FALSE(rv32_result.is_root_success());
  ASSERT_EQ(wasm_result.result_.error().code(), expected);
  EXPECT_EQ(rv32_result.result_.error().code(), expected);
  EXPECT_EQ(wasm_result.result_.error().code(),
            rv32_result.result_.error().code());
}

std::shared_ptr<const Contract>
make_rv32_contract_xdr()
{
  auto contract = std::make_shared<Contract>(ContractType::kMockTest);
  contract->mock_contract_id() = kRv32ContractId;
  return contract;
}

std::shared_ptr<const Contract>
make_wasmi_contract_xdr()
{
  auto contract = std::make_shared<Contract>(ContractType::kWasmi);
  return contract;
}

Transaction
make_root_transaction(GasAmount gas_limit, Runtime::MethodID method_id = 0)
{
  return Transaction{
      .invocation =
          Invocation{
              .invoked_address = kRv32ContractAddress,
              .entrypoint = entrypoint_from_method_id(method_id),
              .gas_limit = gas_limit,
              .calldata = Calldata{},
          },
      .uniquifier =
          Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = 1,
          },
  };
}

class Rv32RuntimeContextTest : public ::testing::Test,
                               public NativeContractTestFramework {
public:
  Rv32RuntimeContextTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void install_contract(std::shared_ptr<const Contract> const &contract)
  {
    write_contract(contract);
    write_deployment(kRv32ContractAddress, contract);
    setup_grant_storage_token(kRv32ContractAddress, 1'000'000'000);
  }
};

void
expect_syscall_trace_suffix_matches(InvocationFrameTrace const &wasm_trace,
                                    InvocationFrameTrace const &rv32_trace)
{
  ASSERT_GE(wasm_trace.system_calls.size(), 2U);
  ASSERT_EQ(rv32_trace.system_calls.size(), 2U);

  const size_t wasm_offset = wasm_trace.system_calls.size() - 2;
  for (size_t i = 0; i < rv32_trace.system_calls.size(); ++i) {
    auto const &wasm_call = wasm_trace.system_calls[wasm_offset + i];
    auto const &rv32_call = rv32_trace.system_calls[i];
    EXPECT_EQ(wasm_call.body.syscall_number(), rv32_call.body.syscall_number());
    EXPECT_EQ(wasm_call.status.error(), rv32_call.status.error());
    if (wasm_call.status.error() == HostFnErrorCode::NoneOrRecoverable) {
      EXPECT_EQ(wasm_call.status.syscall_return_code(),
                rv32_call.status.syscall_return_code());
    }
  }
}

void
expect_sha256_then_return_trace_matches(
    InvocationFrameTrace const &wasm_trace,
    InvocationFrameTrace const &rv32_trace, std::span<const uint8_t> input,
    SHA256Hash const &expected_hash, std::span<const uint8_t> expected_return)
{
  ASSERT_GE(wasm_trace.system_calls.size(), 2U);
  ASSERT_EQ(rv32_trace.system_calls.size(), 2U);
  const size_t wasm_offset = wasm_trace.system_calls.size() - 2;

  auto const &wasm_sha = wasm_trace.system_calls[wasm_offset];
  auto const &rv32_sha = rv32_trace.system_calls[0];
  ASSERT_EQ(wasm_sha.body.syscall_number(), SyscallNo::kSHA256);
  ASSERT_EQ(rv32_sha.body.syscall_number(), SyscallNo::kSHA256);
  EXPECT_EQ(wasm_sha.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(rv32_sha.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(wasm_sha.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(rv32_sha.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_TRUE(std::ranges::equal(wasm_sha.body.sha256().inputs.input, input));
  EXPECT_TRUE(std::ranges::equal(rv32_sha.body.sha256().inputs.input, input));
  ASSERT_NE(wasm_sha.body.sha256().outputs, nullptr);
  ASSERT_NE(rv32_sha.body.sha256().outputs, nullptr);
  EXPECT_EQ(wasm_sha.body.sha256().outputs->hash, expected_hash);
  EXPECT_EQ(rv32_sha.body.sha256().outputs->hash, expected_hash);

  auto const &wasm_return = wasm_trace.system_calls[wasm_offset + 1];
  auto const &rv32_return = rv32_trace.system_calls[1];
  ASSERT_EQ(wasm_return.body.syscall_number(), SyscallNo::kReturn);
  ASSERT_EQ(rv32_return.body.syscall_number(), SyscallNo::kReturn);
  EXPECT_EQ(wasm_return.status.error(), HostFnErrorCode::Return);
  EXPECT_EQ(rv32_return.status.error(), HostFnErrorCode::Return);
  EXPECT_TRUE(std::ranges::equal(wasm_return.body.return_trace().inputs.buffer,
                                 expected_return));
  EXPECT_TRUE(std::ranges::equal(rv32_return.body.return_trace().inputs.buffer,
                                 expected_return));
  EXPECT_FALSE(wasm_return.body.return_trace().inputs.revert_frame);
  EXPECT_FALSE(rv32_return.body.return_trace().inputs.revert_frame);
}

void
expect_sha256_return_trace_matches(InvocationFrameTrace const &wasm_trace,
                                   InvocationFrameTrace const &rv32_trace,
                                   std::span<const uint8_t> input,
                                   SHA256Hash const &expected_hash)
{
  expect_sha256_then_return_trace_matches(wasm_trace, rv32_trace, input,
                                          expected_hash,
                                          std::span<const uint8_t>(
                                              expected_hash));
}

void
expect_storage_event_return_trace_matches(
    InvocationFrameTrace const &wasm_trace,
    InvocationFrameTrace const &rv32_trace)
{
  ASSERT_GE(wasm_trace.system_calls.size(), 3U);
  ASSERT_EQ(rv32_trace.system_calls.size(), 3U);
  const size_t wasm_offset = wasm_trace.system_calls.size() - 3;

  auto const &wasm_set = wasm_trace.system_calls[wasm_offset];
  auto const &rv32_set = rv32_trace.system_calls[0];
  ASSERT_EQ(wasm_set.body.syscall_number(), SyscallNo::kBytestringSet);
  ASSERT_EQ(rv32_set.body.syscall_number(), SyscallNo::kBytestringSet);
  EXPECT_EQ(wasm_set.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(rv32_set.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(wasm_set.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(rv32_set.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(wasm_set.body.bytestring_set().inputs.key,
            rv32_set.body.bytestring_set().inputs.key);
  EXPECT_EQ(wasm_set.body.bytestring_set().inputs.bytestring,
            rv32_set.body.bytestring_set().inputs.bytestring);
  EXPECT_EQ(wasm_set.body.bytestring_set().inputs.flags,
            rv32_set.body.bytestring_set().inputs.flags);

  auto const &wasm_event = wasm_trace.system_calls[wasm_offset + 1];
  auto const &rv32_event = rv32_trace.system_calls[1];
  ASSERT_EQ(wasm_event.body.syscall_number(), SyscallNo::kLogEvent);
  ASSERT_EQ(rv32_event.body.syscall_number(), SyscallNo::kLogEvent);
  EXPECT_EQ(wasm_event.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(rv32_event.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(wasm_event.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(rv32_event.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_NE(wasm_event.body.log_event().inputs.topic0, nullptr);
  ASSERT_NE(rv32_event.body.log_event().inputs.topic0, nullptr);
  EXPECT_EQ(*wasm_event.body.log_event().inputs.topic0,
            *rv32_event.body.log_event().inputs.topic0);
  EXPECT_EQ(wasm_event.body.log_event().inputs.topic1, nullptr);
  EXPECT_EQ(rv32_event.body.log_event().inputs.topic1, nullptr);
  EXPECT_EQ(wasm_event.body.log_event().inputs.topic2, nullptr);
  EXPECT_EQ(rv32_event.body.log_event().inputs.topic2, nullptr);
  EXPECT_EQ(wasm_event.body.log_event().inputs.data,
            rv32_event.body.log_event().inputs.data);

  auto const &wasm_return = wasm_trace.system_calls[wasm_offset + 2];
  auto const &rv32_return = rv32_trace.system_calls[2];
  ASSERT_EQ(wasm_return.body.syscall_number(), SyscallNo::kReturn);
  ASSERT_EQ(rv32_return.body.syscall_number(), SyscallNo::kReturn);
  EXPECT_EQ(wasm_return.status.error(), HostFnErrorCode::Return);
  EXPECT_EQ(rv32_return.status.error(), HostFnErrorCode::Return);
  EXPECT_TRUE(wasm_return.body.return_trace().inputs.buffer.empty());
  EXPECT_TRUE(rv32_return.body.return_trace().inputs.buffer.empty());
  EXPECT_FALSE(wasm_return.body.return_trace().inputs.revert_frame);
  EXPECT_FALSE(rv32_return.body.return_trace().inputs.revert_frame);
}

TEST_F(Rv32RuntimeContextTest, InvalidSyscallReturnMatchesWasmStatusAndTrace)
{
  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<no_syscall_test::NoSyscallTest>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_invalid_syscall_then_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<no_syscall_test::NoSyscallTest> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation = wasm_client.make_invocation<"run_test">(
                      kEnoughGas, static_cast<uint64_t>(kSyscallCount)),
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 1,
                      }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler =
      make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                   rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(wasm_output.return_buffer().size(), 0U);
  EXPECT_EQ(rv32_output.return_buffer().size(), 0U);

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_EQ(wasm_handler.get_trace()->status.status_code(),
            rv32_handler.get_trace()->status.status_code());
  EXPECT_EQ(wasm_handler.get_trace()->status.return_buffer().size(),
            rv32_handler.get_trace()->status.return_buffer().size());
  EXPECT_EQ(wasm_handler.get_trace()->storage_writes.size(),
            rv32_handler.get_trace()->storage_writes.size());
  expect_syscall_trace_suffix_matches(*wasm_handler.get_trace(),
                                      *rv32_handler.get_trace());
  EXPECT_EQ(wasm_handler.get_most_recent_frame_events().size(),
            rv32_handler.get_most_recent_frame_events().size());
  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 1);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 1);
}

TEST_F(Rv32RuntimeContextTest, Sha256ReturnMatchesWasmTraceAndOutput)
{
  const x23::Vec<uint8_t, 0xffffffff> input = {1, 2, 3, 4, 5, 8, 13};
  const SHA256Hash expected_hash = sha256(input);

  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<sha256_wrapper::Sha256Wrapper>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_sha256_then_return_image(input)}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<sha256_wrapper::Sha256Wrapper> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation =
                      wasm_client.make_invocation<"sha256">(kParityGas, input),
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 2,
                      }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(wasm_output.return_buffer(), expected_hash));
  EXPECT_TRUE(std::ranges::equal(rv32_output.return_buffer(), expected_hash));

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_EQ(wasm_handler.get_trace()->status.status_code(),
            rv32_handler.get_trace()->status.status_code());
  EXPECT_TRUE(
      std::ranges::equal(wasm_handler.get_trace()->status.return_buffer(),
                         expected_hash));
  EXPECT_TRUE(
      std::ranges::equal(rv32_handler.get_trace()->status.return_buffer(),
                         expected_hash));
  EXPECT_EQ(wasm_handler.get_trace()->storage_writes.size(),
            rv32_handler.get_trace()->storage_writes.size());
  EXPECT_EQ(wasm_handler.get_most_recent_frame_events().size(),
            rv32_handler.get_most_recent_frame_events().size());
  expect_sha256_return_trace_matches(*wasm_handler.get_trace(),
                                     *rv32_handler.get_trace(), input,
                                     expected_hash);
  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 0);
}

TEST_F(Rv32RuntimeContextTest, CompiledSha256ReturnMatchesWasmTraceAndOutput)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);

  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<sha256_wrapper::Sha256Wrapper>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<sha256_wrapper::Sha256Wrapper> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation =
                      wasm_client.make_invocation<"sha256">(kParityGas, input),
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 4,
                      }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256MethodId),
      rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(wasm_output.return_buffer(), expected_hash));
  EXPECT_TRUE(std::ranges::equal(rv32_output.return_buffer(), expected_hash));

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_EQ(wasm_handler.get_trace()->status.status_code(),
            rv32_handler.get_trace()->status.status_code());
  EXPECT_TRUE(
      std::ranges::equal(wasm_handler.get_trace()->status.return_buffer(),
                         expected_hash));
  EXPECT_TRUE(
      std::ranges::equal(rv32_handler.get_trace()->status.return_buffer(),
                         expected_hash));
  expect_sha256_return_trace_matches(*wasm_handler.get_trace(),
                                     *rv32_handler.get_trace(), input,
                                     expected_hash);
  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 0);
}

TEST_F(Rv32RuntimeContextTest, CompiledMultiMethodDispatchMatchesWasm)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);

  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<sha256_wrapper::Sha256Wrapper>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<sha256_wrapper::Sha256Wrapper> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation = wasm_client.make_invocation<"sha256_repeated">(
                      kParityGas, uint32_t{1}, input),
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 40,
                      }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256RepeatedMethodId),
      rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(wasm_output.return_buffer().empty());
  EXPECT_TRUE(rv32_output.return_buffer().empty());
  ASSERT_FALSE(std::span<const uint8_t>(expected_hash).empty());
  EXPECT_FALSE(std::ranges::equal(wasm_output.return_buffer(), expected_hash));
  EXPECT_FALSE(std::ranges::equal(rv32_output.return_buffer(), expected_hash));

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_EQ(wasm_handler.get_trace()->status.status_code(),
            rv32_handler.get_trace()->status.status_code());
  EXPECT_TRUE(wasm_handler.get_trace()->status.return_buffer().empty());
  EXPECT_TRUE(rv32_handler.get_trace()->status.return_buffer().empty());
  expect_sha256_then_return_trace_matches(*wasm_handler.get_trace(),
                                          *rv32_handler.get_trace(), input,
                                          expected_hash,
                                          std::span<const uint8_t>{});
  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 0);
}

TEST_F(Rv32RuntimeContextTest, StorageWriteAndEventMatchWasmTraceAndEffects)
{
  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<read_only_test::ReadOnlyTest>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_storage_event_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<read_only_test::ReadOnlyTest> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              wasm_client.make_invocation<"write_storage_and_log_event">(
                  kParityGas),
          .uniquifier =
              Uniquifier{
                  .block_expiry_height =
                      std::numeric_limits<BlockHeight>::max(),
                  .nonce = 3,
              }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(wasm_output.return_buffer().empty());
  EXPECT_TRUE(rv32_output.return_buffer().empty());

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  auto const *wasm_trace = wasm_handler.get_trace();
  auto const *rv32_trace = rv32_handler.get_trace();
  const auto wasm_events = wasm_handler.get_most_recent_frame_events();
  const auto rv32_events = rv32_handler.get_most_recent_frame_events();

  ASSERT_EQ(wasm_handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(wasm_handler.emit_events(ExecResult::ExecResultOK)());
  ASSERT_EQ(rv32_handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(rv32_handler.emit_events(ExecResult::ExecResultOK)());

  EXPECT_EQ(wasm_trace->status.status_code(), rv32_trace->status.status_code());
  EXPECT_TRUE(wasm_trace->status.return_buffer().empty());
  EXPECT_TRUE(rv32_trace->status.return_buffer().empty());
  expect_storage_event_return_trace_matches(*wasm_trace, *rv32_trace);

  ASSERT_EQ(wasm_trace->storage_writes.size(), 1U);
  ASSERT_EQ(rv32_trace->storage_writes.size(), 1U);
  auto const &wasm_write = wasm_trace->storage_writes[0];
  auto const &rv32_write = rv32_trace->storage_writes[0];
  EXPECT_EQ(wasm_write.addrkey.address, kWasmContractAddress);
  EXPECT_EQ(rv32_write.addrkey.address, kRv32ContractAddress);
  EXPECT_EQ(wasm_write.addrkey.key, rv32_write.addrkey.key);
  EXPECT_EQ(wasm_write.flags, rv32_write.flags);
  ASSERT_EQ(wasm_write.delta.type(), StorageDeltaType::kDeltaTypeBytestringSet);
  ASSERT_EQ(rv32_write.delta.type(), StorageDeltaType::kDeltaTypeBytestringSet);
  EXPECT_EQ(wasm_write.delta.bytestring(), rv32_write.delta.bytestring());

  ASSERT_EQ(wasm_events.size(), 1U);
  ASSERT_EQ(rv32_events.size(), 1U);
  EXPECT_EQ(wasm_events[0].contract_address, kWasmContractAddress);
  EXPECT_EQ(rv32_events[0].contract_address, kRv32ContractAddress);
  EXPECT_EQ(wasm_events[0].num_topics, 1U);
  EXPECT_EQ(rv32_events[0].num_topics, 1U);
  EXPECT_EQ(wasm_events[0].serialized_event, rv32_events[0].serialized_event);

  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 0);
}

TEST_F(Rv32RuntimeContextTest, Sha256ReturnIsDeterministicForSameInputs)
{
  const x23::Vec<uint8_t, 0xffffffff> input = {3, 1, 4, 1, 5, 9};
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_sha256_then_return_image(input)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler1 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  auto handler2 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  handler1.enable_tracing();
  handler2.enable_tracing();
  const auto result1 = handler1.root_invoke();
  const auto result2 = handler2.root_invoke();

  ASSERT_TRUE(result1.is_root_success());
  ASSERT_TRUE(result2.is_root_success());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);

  const auto output1 = handler1.get_most_recent_frame_output();
  const auto output2 = handler2.get_most_recent_frame_output();
  EXPECT_EQ(output1.status_code(), output2.status_code());
  EXPECT_TRUE(std::ranges::equal(output1.return_buffer(), expected_hash));
  EXPECT_TRUE(
      std::ranges::equal(output1.return_buffer(), output2.return_buffer()));

  ASSERT_NE(handler1.get_trace(), nullptr);
  ASSERT_NE(handler2.get_trace(), nullptr);
  EXPECT_EQ(handler1.get_trace()->system_calls.size(),
            handler2.get_trace()->system_calls.size());
  expect_sha256_return_trace_matches(*handler1.get_trace(),
                                     *handler2.get_trace(), input,
                                     expected_hash);
  for (size_t i = 0; i < handler1.get_trace()->system_calls.size(); ++i) {
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_start,
              handler2.get_trace()->system_calls[i].gas_remaining_at_start);
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_end,
              handler2.get_trace()->system_calls[i].gas_remaining_at_end);
  }
}

TEST_F(Rv32RuntimeContextTest, CompiledSha256ReturnIsDeterministicForSameInputs)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler1 = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256MethodId),
      rv32_context);
  auto handler2 = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256MethodId),
      rv32_context);
  handler1.enable_tracing();
  handler2.enable_tracing();
  const auto result1 = handler1.root_invoke();
  const auto result2 = handler2.root_invoke();

  ASSERT_TRUE(result1.is_root_success());
  ASSERT_TRUE(result2.is_root_success());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);

  const auto output1 = handler1.get_most_recent_frame_output();
  const auto output2 = handler2.get_most_recent_frame_output();
  EXPECT_EQ(output1.status_code(), output2.status_code());
  EXPECT_TRUE(std::ranges::equal(output1.return_buffer(), expected_hash));
  EXPECT_TRUE(
      std::ranges::equal(output1.return_buffer(), output2.return_buffer()));

  ASSERT_NE(handler1.get_trace(), nullptr);
  ASSERT_NE(handler2.get_trace(), nullptr);
  EXPECT_EQ(handler1.get_trace()->system_calls.size(),
            handler2.get_trace()->system_calls.size());
  expect_sha256_return_trace_matches(*handler1.get_trace(),
                                     *handler2.get_trace(), input,
                                     expected_hash);
  for (size_t i = 0; i < handler1.get_trace()->system_calls.size(); ++i) {
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_start,
              handler2.get_trace()->system_calls[i].gas_remaining_at_start);
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_end,
              handler2.get_trace()->system_calls[i].gas_remaining_at_end);
  }
}

TEST_F(Rv32RuntimeContextTest,
       CompiledMultiMethodDispatchIsDeterministicForSameInputs)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler1 = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256RepeatedMethodId),
      rv32_context);
  auto handler2 = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256RepeatedMethodId),
      rv32_context);
  handler1.enable_tracing();
  handler2.enable_tracing();
  const auto result1 = handler1.root_invoke();
  const auto result2 = handler2.root_invoke();

  ASSERT_TRUE(result1.is_root_success());
  ASSERT_TRUE(result2.is_root_success());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);

  const auto output1 = handler1.get_most_recent_frame_output();
  const auto output2 = handler2.get_most_recent_frame_output();
  EXPECT_EQ(output1.status_code(), output2.status_code());
  EXPECT_TRUE(output1.return_buffer().empty());
  EXPECT_TRUE(output2.return_buffer().empty());

  ASSERT_NE(handler1.get_trace(), nullptr);
  ASSERT_NE(handler2.get_trace(), nullptr);
  ASSERT_EQ(handler1.get_trace()->system_calls.size(),
            handler2.get_trace()->system_calls.size());
  expect_sha256_then_return_trace_matches(*handler1.get_trace(),
                                          *handler2.get_trace(), input,
                                          expected_hash,
                                          std::span<const uint8_t>{});
  for (size_t i = 0; i < handler1.get_trace()->system_calls.size(); ++i) {
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_start,
              handler2.get_trace()->system_calls[i].gas_remaining_at_start);
    EXPECT_EQ(handler1.get_trace()->system_calls[i].gas_remaining_at_end,
              handler2.get_trace()->system_calls[i].gas_remaining_at_end);
  }
}

TEST_F(Rv32RuntimeContextTest,
       ArtifactArithmeticControlFlowReturnsDeterministicBytes)
{
  const std::array<uint8_t, 4> expected = {15, 0, 0, 0};
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_artifact_arithmetic_control_flow_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler1 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  auto handler2 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  handler1.enable_tracing();
  handler2.enable_tracing();

  const auto result1 = handler1.root_invoke();
  const auto result2 = handler2.root_invoke();

  ASSERT_TRUE(result1.is_root_success());
  ASSERT_TRUE(result2.is_root_success());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);
  const auto output1 = handler1.get_most_recent_frame_output();
  const auto output2 = handler2.get_most_recent_frame_output();
  ASSERT_EQ(output1.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(output2.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output1.return_buffer(), expected));
  EXPECT_TRUE(
      std::ranges::equal(output1.return_buffer(), output2.return_buffer()));
  ASSERT_NE(handler1.get_trace(), nullptr);
  ASSERT_NE(handler2.get_trace(), nullptr);
  EXPECT_EQ(handler1.get_trace()->system_calls.size(), 1U);
  EXPECT_EQ(handler2.get_trace()->system_calls.size(), 1U);
}

TEST_F(Rv32RuntimeContextTest, ArtifactMemoryTransformReturnsDeterministicBytes)
{
  std::array<uint8_t, 16> expected = {};
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(0x31U + i);
  }
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_artifact_memory_transform_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler1 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);
  auto handler2 =
      make_new_system_call_handler(make_root_transaction(kParityGas),
                                   rv32_context);

  const auto result1 = handler1.root_invoke();
  const auto result2 = handler2.root_invoke();

  ASSERT_TRUE(result1.is_root_success());
  ASSERT_TRUE(result2.is_root_success());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);
  const auto output1 = handler1.get_most_recent_frame_output();
  const auto output2 = handler2.get_most_recent_frame_output();
  ASSERT_EQ(output1.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(output2.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output1.return_buffer(), expected));
  EXPECT_TRUE(
      std::ranges::equal(output1.return_buffer(), output2.return_buffer()));
}

TEST_F(Rv32RuntimeContextTest, ArtifactMissingDispatchMethodIsDeterministic)
{
  const std::array<uint8_t, 1> payload = {0};
  auto image = make_artifact_runtime_image(
      {
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      },
      make_initial_return_memory(payload), {{.method_id = 1, .pc = 0}});
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);
  auto handler = make_new_system_call_handler(make_root_transaction(kParityGas),
                                              rv32_context);
  handler.enable_tracing();

  const auto result = handler.root_invoke();

  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
  EXPECT_EQ(result.gas_consumed_, 0U);
  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->system_calls.empty());
}

TEST_F(Rv32RuntimeContextTest, DispatchesNonzeroMethodToSelectedPc)
{
  const std::array<uint8_t, 4> zero_payload = {'z', 'e', 'r', 'o'};
  const std::array<uint8_t, 3> one_payload = {'o', 'n', 'e'};

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId,
        make_two_method_return_image(zero_payload, one_payload)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);
  auto handler =
      make_new_system_call_handler(make_root_transaction(kEnoughGas, 1),
                                   rv32_context);

  auto result = handler.root_invoke();

  ASSERT_TRUE(result.is_root_success());
  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), one_payload));
}

TEST_F(Rv32RuntimeContextTest, UnknownDispatchMethodIsDeterministicMiss)
{
  constexpr Runtime::MethodID kUnknownMethodId = 2;
  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<sha256_wrapper::Sha256Wrapper>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{
                          .invoked_address = kWasmContractAddress,
                          .entrypoint =
                              entrypoint_from_method_id(kUnknownMethodId),
                          .gas_limit = kEnoughGas,
                          .calldata = Calldata{},
                      },
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 41,
                      }},
      *wasm_context);
  auto rv32_handler = make_new_system_call_handler(
      make_root_transaction(kEnoughGas, kUnknownMethodId), rv32_context);
  rv32_handler.enable_tracing();

  auto wasm_result = wasm_handler.root_invoke();
  auto rv32_result = rv32_handler.root_invoke();

  ASSERT_FALSE(wasm_result.is_root_success());
  ASSERT_FALSE(rv32_result.is_root_success());
  EXPECT_EQ(wasm_result.result_.error().code(),
            rv32_result.result_.error().code());
  ASSERT_EQ(rv32_result.result_.error().code(), InvokeErrorCode::Deterministic);
  EXPECT_EQ(wasm_result.gas_consumed_, 0U);
  EXPECT_EQ(rv32_result.gas_consumed_, 0U);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_TRUE(rv32_handler.get_trace()->system_calls.empty());
}

TEST_F(Rv32RuntimeContextTest, RootInvokeReturnsBufferThroughSystemCallHandler)
{
  const std::array<uint8_t, 4> payload = {'r', 'v', '3', '2'};
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, make_return_image(payload)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  auto result = handler.root_invoke();

  ASSERT_TRUE(result.is_root_success());
  EXPECT_EQ(result.gas_consumed_, 3U * rv32::kBaseInstructionGas);
  auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(output.return_buffer().size(), payload.size());
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), payload));
}

TEST_F(Rv32RuntimeContextTest, DirectInvokeNormalizesOutOfGasConsumption)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_single_ecall_image(&host_out_of_gas_entry)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);

  auto result = direct_invoke(contract, rv32_context, handler, 10);

  ASSERT_FALSE(result.is_root_success());
  ASSERT_EQ(result.result_.error().code(), InvokeErrorCode::OutOfGas);
  EXPECT_EQ(result.gas_consumed_, 10U);
}

TEST_F(Rv32RuntimeContextTest, HostErrorContractsMatchWasmOutcomes)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_parity = [&](HostFn0 wasm_entry, HostFn11 rv32_entry,
                           InvokeErrorCode expected) {
    for (const WasmEngine engine : kDifferentialWasmEngines) {
      auto rv32_context = rv32::RuntimeContext(
          {{kRv32ContractId, make_single_ecall_image(rv32_entry)}});
      auto handler =
          make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                       rv32_context);

      const auto wasm_result =
          invoke_wasm_error_contract(engine, wasm_entry, "call1", kEnoughGas);
      const auto rv32_result =
          direct_invoke(contract, rv32_context, handler, kEnoughGas);

      expect_matching_error(wasm_result, rv32_result, expected);
    }
  };

  expect_parity(&wasm_unrecoverable_entry, &unrecoverable_entry,
                InvokeErrorCode::Unrecoverable);
  expect_parity(&wasm_validation_failure_entry, &validation_failure_entry,
                InvokeErrorCode::ValidationFailure);
  expect_parity(&wasm_abandon_entry, &abandon_entry, InvokeErrorCode::Abandon);
}

TEST_F(Rv32RuntimeContextTest, DeterministicTrapContractMatchesWasmOutcome)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  for (const WasmEngine engine : kDifferentialWasmEngines) {
    auto rv32_context =
        rv32::RuntimeContext({{kRv32ContractId, make_bad_pc_image()}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);

    const auto wasm_result =
        invoke_wasm_error_contract(engine, &wasm_good_call_entry, "unreachable",
                                   kEnoughGas);
    const auto rv32_result =
        direct_invoke(contract, rv32_context, handler, kEnoughGas);

    expect_matching_error(wasm_result, rv32_result,
                          InvokeErrorCode::Deterministic);
  }
}

TEST_F(Rv32RuntimeContextTest, OutOfGasContractMatchesWasmOutcome)
{
  constexpr GasAmount kGasLimit = 10;
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  for (const WasmEngine engine : kDifferentialWasmEngines) {
    auto rv32_context = rv32::RuntimeContext(
        {{kRv32ContractId, make_single_ecall_image(&host_out_of_gas_entry)}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kGasLimit),
                                     rv32_context);

    const auto wasm_result =
        invoke_wasm_error_contract(engine, &wasm_consume_too_much_gas_entry,
                                   "call1", kGasLimit);
    const auto rv32_result =
        direct_invoke(contract, rv32_context, handler, kGasLimit);

    expect_matching_error(wasm_result, rv32_result, InvokeErrorCode::OutOfGas);
    EXPECT_EQ(wasm_result.gas_consumed_, kGasLimit);
    EXPECT_EQ(rv32_result.gas_consumed_, kGasLimit);
  }
}

TEST_F(Rv32RuntimeContextTest, HostTrapPanicCodesMapToWasmInvokeOutcomes)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_mapping = [&](HostFn11 entry, InvokeErrorCode expected) {
    auto rv32_context = rv32::RuntimeContext(
        {{kRv32ContractId, make_single_ecall_image(entry)}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);
    auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

    ASSERT_FALSE(result.is_root_success());
    EXPECT_EQ(result.result_.error().code(), expected);
    EXPECT_EQ(result.gas_consumed_, (3U * rv32::kBaseInstructionGas) + 4U);
  };

  expect_mapping(&unrecoverable_entry, InvokeErrorCode::Unrecoverable);
  expect_mapping(&validation_failure_entry, InvokeErrorCode::ValidationFailure);
  expect_mapping(&abandon_entry, InvokeErrorCode::Abandon);
  expect_mapping(&unknown_panic_entry, InvokeErrorCode::Unrecoverable);
}

TEST_F(Rv32RuntimeContextTest, BridgeAbiFaultsMapToDeterministic)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_deterministic = [&](rv32::RuntimeContractImage image) {
    auto rv32_context =
        rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);
    auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

    ASSERT_FALSE(result.is_root_success());
    EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
    EXPECT_EQ(result.gas_consumed_, 3U * rv32::kBaseInstructionGas);
  };

  expect_deterministic(
      make_bridge_abi_fault_image(kFrameAddress,
                                  rv32::kSyscallBridgeVersion + 1));
  expect_deterministic(
      make_bridge_abi_fault_image(kFrameAddress + 1,
                                  rv32::kSyscallBridgeVersion));
  expect_deterministic(
      make_bridge_abi_fault_image(200, rv32::kSyscallBridgeVersion));
}

TEST_F(Rv32RuntimeContextTest, FatalCodeZeroHostTrapMapsToUnrecoverable)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_single_ecall_image(&throwing_entry)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);

  auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Unrecoverable);
  EXPECT_EQ(result.gas_consumed_, 3U * rv32::kBaseInstructionGas);
}

TEST_F(Rv32RuntimeContextTest, ZeroReturnHaltCodeMapsToSuccess)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_single_ecall_image(&return_entry)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);

  auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

  ASSERT_TRUE(result.is_root_success());
  EXPECT_EQ(result.gas_consumed_, (3U * rv32::kBaseInstructionGas) + 4U);
}

TEST_F(Rv32RuntimeContextTest, NonzeroExitHaltCodeMapsToDeterministic)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_single_ecall_image(&exit_entry)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);

  auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
}

TEST_F(Rv32RuntimeContextTest, GuestTrapsMapToDeterministic)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_deterministic = [&](rv32::RuntimeContractImage image) {
    auto rv32_context =
        rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);
    auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

    ASSERT_FALSE(result.is_root_success());
    EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
  };

  expect_deterministic(make_bad_pc_image());
  expect_deterministic(make_bad_memory_image());
  expect_deterministic(make_misaligned_access_image());
}

TEST_F(Rv32RuntimeContextTest, SameInputsTrapDeterministically)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, make_bad_memory_image()}});

  auto handler1 =
      make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                   rv32_context);
  auto handler2 =
      make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                   rv32_context);

  auto result1 = direct_invoke(contract, rv32_context, handler1, kEnoughGas);
  auto result2 = direct_invoke(contract, rv32_context, handler2, kEnoughGas);

  ASSERT_FALSE(result1.is_root_success());
  ASSERT_FALSE(result2.is_root_success());
  EXPECT_EQ(result1.result_.error().code(), result2.result_.error().code());
  EXPECT_EQ(result1.gas_consumed_, result2.gas_consumed_);
}

TEST_F(Rv32RuntimeContextTest, VerifyRejectsUndefinedRv32MockContract)
{
  auto rv32_context = rv32::RuntimeContext({});
  auto contract = make_rv32_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);
}

TEST_F(Rv32RuntimeContextTest, VerifyRejectsNonMockContractWithoutAbort)
{
  auto rv32_context = rv32::RuntimeContext({});
  auto contract = make_wasmi_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);
}

TEST_F(Rv32RuntimeContextTest, InvalidRuntimeDispatchTableIsRejected)
{
  const std::array<uint8_t, 4> payload = {'r', 'v', '3', '2'};
  auto image = make_return_image(payload);
  image.dispatch_table = {
      {.method_id = 1, .pc = 0},
      {.method_id = 0, .pc = 0},
  };
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Unrecoverable);
  EXPECT_EQ(result.gas_consumed_, 0U);
}

// SPIKE smoke test for SDK-lowered RV32 artifacts.
//
// Set RV32_SMOKE_ARTIFACT to a `.rv32a` path produced by
// scripts/rv32_lower.py against an sdk_rs counter-rv32 ELF. The test
// parses, validates, and invokes the first dispatch entry, and prints
// the observed outcome. It's intentionally informational — EXPECT, not
// ASSERT — so a failure here surfaces the actual VM/SDK gap rather
// than just printing a green/red ctest line.
//
// Until cross-repo CI builds the SDK contract automatically, the env
// var is the cleanest way to plumb the artifact in without committing
// a binary blob to the hogd tree.
TEST_F(Rv32RuntimeContextTest, LoweredSdkArtifactSmoke)
{
  const char *artifact_path = std::getenv("RV32_SMOKE_ARTIFACT");
  if (artifact_path == nullptr) {
    GTEST_SKIP() << "set RV32_SMOKE_ARTIFACT=/path/to/contract.rv32a to run "
                    "this end-to-end smoke test";
  }

  std::ifstream input(artifact_path, std::ios::binary);
  ASSERT_TRUE(input.is_open()) << "cannot open " << artifact_path;
  std::vector<uint8_t> artifact_bytes(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
  ASSERT_GT(artifact_bytes.size(), 0U);
  std::cerr << "[smoke] loaded artifact: " << artifact_bytes.size()
            << " bytes from " << artifact_path << "\n";

  auto parsed = rv32::parse_artifact(artifact_bytes);
  ASSERT_TRUE(parsed.has_value())
      << "parse_artifact rejected the lowered artifact (error="
      << static_cast<int>(parsed.error()) << ")";
  std::cerr << "[smoke] parsed: code_bytes=" << parsed->program->byte_size()
            << " dispatch_entries=" << parsed->dispatch_table.size()
            << " memory_bytes=" << parsed->memory_bytes << "\n";
  ASSERT_FALSE(parsed->dispatch_table.empty());

  rv32::RuntimeContractImage image{
      .program = parsed->program,
      .initial_memory = std::move(parsed->initial_memory),
      .dispatch_table = std::move(parsed->dispatch_table),
      .memory_bytes = parsed->memory_bytes,
  };

  // Pick the lowest-ID dispatch entry as the smoke target. For counter
  // this lands on whichever of initialize/increment/decrement/get
  // sorts first by ContractEntrypointID hash.
  const auto smoke_entry = image.dispatch_table.front();
  std::cerr << "[smoke] invoking method_id=0x" << std::hex
            << smoke_entry.method_id << " at pc=" << std::dec << smoke_entry.pc
            << "\n";

  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(
      make_root_transaction(kParityGas, smoke_entry.method_id), rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  const auto result = handler.root_invoke();

  if (handler.get_trace() != nullptr) {
    std::cerr << "[smoke] trace status_code="
              << static_cast<int>(handler.get_trace()->status.status_code())
              << " syscalls=" << handler.get_trace()->system_calls.size()
              << "\n";
  }
  const auto frame_output = handler.get_most_recent_frame_output();
  std::cerr << "[smoke] frame status_code="
            << static_cast<int>(frame_output.status_code())
            << " return_bytes=" << frame_output.return_buffer().size() << "\n";
  if (handler.get_transaction_stats() != nullptr) {
    std::cerr << "[smoke] stats invalid_syscall_count="
              << handler.get_transaction_stats()->invalid_syscall_count
              << "\n";
  }

  // The interesting signal: did the macro-emitted entry point actually
  // execute as a VM entry point? Success means the lowerer + macro +
  // runtime composition works end-to-end. A non-success outcome is
  // expected to be informative (which kind of failure?), not green.
  EXPECT_TRUE(result.is_root_success())
      << "lowered artifact did not return cleanly — check trace status above";
}

} // namespace
} // namespace hogd

// NOLINTEND(readability-named-parameter,misc-multiple-inheritance)
