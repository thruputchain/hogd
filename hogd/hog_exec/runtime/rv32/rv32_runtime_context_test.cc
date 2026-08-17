#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/no_syscall_test.h"
#include "built_contracts/read_only_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/json.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/fixtures/compiled_sha256_return/compiled_sha256_return_fixture.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/runtime_context.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/hog_exec/runtime/trampoline_result.h"
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
constexpr uint32_t kReturnBufferAddress = 192;
constexpr uint32_t kShaInputAddress = 224;
constexpr uint32_t kShaOutputAddress = 320;
constexpr uint32_t kShaMemoryBytes = 512;
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
constexpr std::u8string_view kRv32SmokeCounterInterface = u8"Counter";
// Keep in sync with built_contracts/integration_test_contracts/
// rv32_smoke_counter/Cargo.toml; the SDK-lowered round-trip tests below fail
// loudly if the version drifts from method-id hashing inputs.
constexpr std::u8string_view kRv32SmokeCounterVersion = u8"0.2.0";

struct LoadedRv32SmokeArtifact {
  std::string path;
  size_t artifact_size;
  rv32::RuntimeContractImage image;
};

std::string
get_lowered_sdk_smoke_artifact_path()
{
  if (const char *artifact_path = std::getenv("RV32_SMOKE_ARTIFACT");
      artifact_path != nullptr && artifact_path[0] != '\0') {
    return artifact_path;
  }

#ifdef HOGD_RV32_GENERATED_SMOKE_ARTIFACT
  return HOGD_RV32_GENERATED_SMOKE_ARTIFACT;
#else
  return {};
#endif
}

std::string
get_lowered_sdk_smoke_metadata_path()
{
  if (const char *metadata_path = std::getenv("RV32_SMOKE_METADATA");
      metadata_path != nullptr && metadata_path[0] != '\0') {
    return metadata_path;
  }

#ifdef HOGD_RV32_GENERATED_SMOKE_METADATA
  return HOGD_RV32_GENERATED_SMOKE_METADATA;
#else
  return {};
#endif
}

std::string
artifact_format_fingerprint_hex()
{
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0') << std::setw(16)
      << rv32::kArtifactFormatFingerprint;
  return out.str();
}

Runtime::MethodID
rv32_smoke_counter_method_id(std::u8string_view method)
{
  return sdk_method_id(kRv32SmokeCounterInterface, kRv32SmokeCounterVersion,
                       method);
}

std::optional<LoadedRv32SmokeArtifact>
load_lowered_sdk_smoke_artifact(std::string artifact_path)
{
  std::ifstream input(artifact_path, std::ios::binary);
  if (!input.is_open()) {
    ADD_FAILURE() << "cannot open " << artifact_path;
    return std::nullopt;
  }
  std::vector<uint8_t> artifact_bytes((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
  if (artifact_bytes.empty()) {
    ADD_FAILURE() << "empty artifact at " << artifact_path;
    return std::nullopt;
  }

  auto parsed = rv32::parse_artifact(artifact_bytes);
  if (!parsed.has_value()) {
    ADD_FAILURE() << "parse_artifact rejected the lowered artifact (error="
                  << static_cast<int>(parsed.error()) << ")";
    return std::nullopt;
  }

  return LoadedRv32SmokeArtifact{
      .path = std::move(artifact_path),
      .artifact_size = artifact_bytes.size(),
      .image =
          rv32::RuntimeContractImage{
              .program = parsed->program,
              .initial_memory = std::move(parsed->initial_memory),
              .memory_bytes = parsed->memory_bytes,
          },
  };
}

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

std::shared_ptr<const rv32::Program>
make_program(const std::vector<uint32_t> &words)
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

// Append the register-native syscall sequence: "addi a(0+i), 0, arg_i" for each
// argument (a0 carries the syscall number) followed by ecall. Immediates must
// fit addi's signed 12-bit field; -1 yields 0xFFFFFFFF, the optional-pointer
// None sentinel in the shared i32 syscall ABI.
void
append_syscall(std::vector<uint32_t> &program,
               std::initializer_list<int32_t> arg_regs)
{
  uint8_t reg = 10; // a0
  for (const int32_t imm : arg_regs) {
    program.push_back(addi(reg, 0, imm));
    ++reg;
  }
  program.push_back(ecall());
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
                    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Unrecoverable);
}

extern "C" TrampolineResult
validation_failure_entry(ExternHostCallContext /*unused*/,
                         WasmContext /*unused*/, uint32_t, uint32_t, uint32_t,
                         uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::ValidationFailure);
}

extern "C" TrampolineResult
abandon_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
              uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Abandon);
}

extern "C" TrampolineResult
unknown_panic_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t)
{
  return {.result_code = 0, .panic = 999, .gas_consumed = 4};
}

extern "C" TrampolineResult
throwing_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
               uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
               uint32_t, uint32_t)
{
  throw 1;
}

extern "C" TrampolineResult
host_out_of_gas_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                      uint32_t, uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::OutOfGas, 0);
}

extern "C" TrampolineResult
exit_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t,
           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
  return make_exit_result(17);
}

extern "C" TrampolineResult
zero_code_exit_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                     uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t, uint32_t)
{
  return make_exit_result(0);
}

extern "C" TrampolineResult
return_entry(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t,
             uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
             uint32_t)
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

// Stage the Return payload at kReturnBufferAddress. Under the register-native
// ABI the Return syscall arguments (buffer pointer/length) are loaded into
// registers by the program, not written to a memory frame.
void
write_return_payload(std::span<uint8_t> memory,
                     std::span<const uint8_t> payload)
{
  std::ranges::copy(payload, memory.begin() + kReturnBufferAddress);
}

std::vector<uint8_t>
make_initial_return_memory(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  write_return_payload(initial_memory, payload);
  return initial_memory;
}

// Emit the register-native Return syscall returning `length` bytes from
// kReturnBufferAddress. Only a0..a2 are loaded; the remaining Return arguments
// (revert flag, owned-auth-token span) default to zero in freshly-zeroed
// machine registers.
void
append_return(std::vector<uint32_t> &program, uint32_t length)
{
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kReturn),
                           static_cast<int32_t>(kReturnBufferAddress),
                           static_cast<int32_t>(length)});
}

rv32::RuntimeContractImage
make_single_ecall_image(HostFn8 entry)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  return rv32::RuntimeContractImage{
      // Mock host entries ignore their arguments, so any three-instruction
      // ecall body suffices (the gas accounting depends on this length).
      .program = make_program({
          addi(10, 0, 0),
          addi(11, 0, 0),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
      .entry = entry,
  };
}

rv32::RuntimeContractImage
make_return_image(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory = make_initial_return_memory(payload);
  std::vector<uint32_t> program;
  append_return(program, payload.size());
  return rv32::RuntimeContractImage{
      .program = make_program(program),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
  };
}

// v4 guest-side dispatch: _start reads the seeded method id (a0 = low 32 bits)
// and branches. Method 1 returns one_payload; any other id returns zero_payload
// (the default arm), mirroring how an SDK-generated dispatcher routes by id.
rv32::RuntimeContractImage
make_two_method_return_image(std::span<const uint8_t> zero_payload,
                             std::span<const uint8_t> one_payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  constexpr uint32_t kZeroReturnBufferAddress = 192;
  constexpr uint32_t kOneReturnBufferAddress = 224;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  std::ranges::copy(zero_payload,
                    initial_memory.begin() + kZeroReturnBufferAddress);
  std::ranges::copy(one_payload,
                    initial_memory.begin() + kOneReturnBufferAddress);

  // x6 = 1; if a0 (method id low) != 1, branch +20 bytes to the zero-method
  // Return block; otherwise fall through to the one-method Return block. Each
  // Return block is a0=kReturn, a1=buffer, a2=len, ecall (Return halts, so the
  // one-method block never falls into the zero block).
  const std::vector<uint32_t> program = {
      addi(6, 0, 1),
      rv32::test_program::branch(1, 10, 6, 20), // bne a0, x6, +20
      addi(10, 0, static_cast<int32_t>(SyscallNo::kReturn)),
      addi(11, 0, static_cast<int32_t>(kOneReturnBufferAddress)),
      addi(12, 0, static_cast<int32_t>(one_payload.size())),
      ecall(),
      addi(10, 0, static_cast<int32_t>(SyscallNo::kReturn)),
      addi(11, 0, static_cast<int32_t>(kZeroReturnBufferAddress)),
      addi(12, 0, static_cast<int32_t>(zero_payload.size())),
      ecall(),
  };

  return rv32::RuntimeContractImage{
      .program = make_program(program),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_sha256_then_return_image(std::span<const uint8_t> input)
{
  std::vector<uint8_t> initial_memory(kShaMemoryBytes);
  std::ranges::copy(input, initial_memory.begin() + kShaInputAddress);

  std::vector<uint32_t> program;
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kSHA256),
                           static_cast<int32_t>(kShaInputAddress),
                           static_cast<int32_t>(input.size()),
                           static_cast<int32_t>(kShaOutputAddress)});
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kReturn),
                           static_cast<int32_t>(kShaOutputAddress),
                           static_cast<int32_t>(SHA256Hash{}.size()), 0, 0, 0});

  return rv32::RuntimeContractImage{
      .program = make_program(program),
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
      .memory_bytes = artifact->memory_bytes,
  };
}

rv32::RuntimeContractImage
make_artifact_runtime_image(std::initializer_list<uint32_t> words,
                            std::vector<uint8_t> initial_memory)
{
  const std::vector<uint8_t> artifact =
      rv32::test_artifact::build_artifact(words, initial_memory);
  auto parsed = rv32::parse_artifact(artifact);
  if (!parsed.has_value()) {
    ADD_FAILURE() << "failed to parse synthetic RV32 artifact";
    return {};
  }
  return rv32::RuntimeContractImage{
      .program = std::move(parsed->program),
      .initial_memory = std::move(parsed->initial_memory),
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
          // Return the computed word: a0=kReturn, a1=buffer, a2=length.
          addi(10, 0, static_cast<int32_t>(SyscallNo::kReturn)),
          addi(11, 0, kReturnBufferAddress),
          addi(12, 0, static_cast<int32_t>(return_placeholder.size())),
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
          // Return the copied region: a0=kReturn, a1=buffer, a2=length.
          addi(10, 0, static_cast<int32_t>(SyscallNo::kReturn)),
          addi(11, 0, kReturnBufferAddress),
          addi(12, 0, kCopyBytes),
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

  std::vector<uint32_t> program;
  // BytestringSet(key, value, value_len, flags).
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kBytestringSet),
                           static_cast<int32_t>(kEffectStorageKeyAddress),
                           static_cast<int32_t>(kEffectStorageValueAddress),
                           static_cast<int32_t>(kEffectStorageValue.size()),
                           static_cast<int32_t>(StorageDeltaFlags::None)});
  // LogEvent(topic0, topic1=None, topic2=None, data, data_len). The -1 register
  // values produce the UINT32_MAX optional-pointer None sentinel.
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kLogEvent),
                           static_cast<int32_t>(kEffectTopicAddress), -1, -1,
                           static_cast<int32_t>(kEffectEventDataAddress),
                           static_cast<int32_t>(kEffectEventData.size())});
  // Return with an empty buffer.
  append_syscall(program,
                 {static_cast<int32_t>(SyscallNo::kReturn), 0, 0, 0, 0, 0});

  return rv32::RuntimeContractImage{
      .program = make_program(program),
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
  // lw at 256: 4-byte aligned but entirely outside [0,256) -- aligned fully-OOB
  // variant; sibling make_unaligned_oob_access_image covers unaligned
  // end-overlap.
  return rv32::RuntimeContractImage{
      .program = make_program({
          i_type(0x03, 1, 2, 0, 256),
      }),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_unaligned_oob_access_image()
{
  // lw at 253: in [0,256) but 253+4 > 256 -- unaligned AND out of bounds,
  // which traps kBadMemory now that unaligned in-bounds access is legal.
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(1, 0, 253),
          i_type(0x03, 2, 2, 1, 0),
      }),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_breakpoint_image()
{
  return rv32::RuntimeContractImage{
      .program = make_program({0x00100073U}),
      .initial_memory = std::vector<uint8_t>(256),
      .memory_bytes = 256,
  };
}

rv32::RuntimeContractImage
make_invalid_syscall_then_return_image()
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  std::vector<uint32_t> program;
  // An out-of-range syscall number falls back to NoSyscall (continue); the
  // contract then terminates with an empty Return.
  append_syscall(program, {static_cast<int32_t>(kSyscallCount)});
  append_syscall(program,
                 {static_cast<int32_t>(SyscallNo::kReturn), 0, 0, 0, 0, 0});
  return rv32::RuntimeContractImage{
      .program = make_program(program),
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
make_non_mock_contract_xdr()
{
  auto contract = std::make_shared<Contract>(ContractType::kRv32);
  return contract;
}

Transaction
make_root_transaction(GasAmount gas_limit, Runtime::MethodID method_id = 0,
                      Calldata calldata = Calldata{}, uint64_t nonce = 1)
{
  return Transaction{
      .invocation =
          Invocation{
              .invoked_address = kRv32ContractAddress,
              .entrypoint = entrypoint_from_method_id(method_id),
              .gas_limit = gas_limit,
              .calldata = std::move(calldata),
          },
      .uniquifier =
          Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = nonce,
          },
  };
}

class Rv32RuntimeContextTest : public ::testing::Test,
                               public NativeContractTestFramework {
public:
  Rv32RuntimeContextTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
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
  auto wasm_context = make_hog_exec_context();
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
  SignedTransaction wasm_handler_tx{
      .tx = Transaction{.invocation = wasm_client.make_invocation<u8"run_test">(
                            kEnoughGas, static_cast<uint64_t>(kSyscallCount)),
                        .uniquifier = Uniquifier{
                            .block_expiry_height =
                                std::numeric_limits<BlockHeight>::max(),
                            .nonce = 1,
                        }}};
  auto wasm_handler =
      make_new_system_call_handler(wasm_handler_tx.tx, *wasm_context);
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

  auto wasm_context = make_hog_exec_context();
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
  SignedTransaction wasm_handler_tx{
      .tx = Transaction{
          .invocation =
              wasm_client.make_invocation<u8"sha256">(kParityGas, input),
          .uniquifier = Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = 2,
          }}};
  auto wasm_handler =
      make_new_system_call_handler(wasm_handler_tx.tx, *wasm_context);
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

  auto wasm_context = make_hog_exec_context();
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
  SignedTransaction wasm_handler_tx{
      .tx = Transaction{
          .invocation =
              wasm_client.make_invocation<u8"sha256">(kParityGas, input),
          .uniquifier = Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = 4,
          }}};
  auto wasm_handler =
      make_new_system_call_handler(wasm_handler_tx.tx, *wasm_context);
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

  auto wasm_context = make_hog_exec_context();
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
  SignedTransaction wasm_handler_tx{
      .tx = Transaction{.invocation =
                            wasm_client.make_invocation<u8"sha256_repeated">(
                                kParityGas, uint32_t{1}, input),
                        .uniquifier = Uniquifier{
                            .block_expiry_height =
                                std::numeric_limits<BlockHeight>::max(),
                            .nonce = 40,
                        }}};
  auto wasm_handler =
      make_new_system_call_handler(wasm_handler_tx.tx, *wasm_context);
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
  auto wasm_context = make_hog_exec_context();
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
  SignedTransaction wasm_handler_tx{
      .tx = Transaction{
          .invocation =
              wasm_client.make_invocation<u8"write_storage_and_log_event">(
                  kParityGas),
          .uniquifier = Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = 3,
          }}};
  auto wasm_handler =
      make_new_system_call_handler(wasm_handler_tx.tx, *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  SignedTransaction rv32_handler_tx{.tx = make_root_transaction(kParityGas)};
  auto rv32_handler =
      make_new_system_call_handler(rv32_handler_tx.tx, rv32_context);
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
  ASSERT_TRUE(wasm_handler.emit_events(ExecResult::ExecResultOK,
                                       ProposedTransaction{
                                           .signed_tx = wasm_handler_tx})());
  ASSERT_EQ(rv32_handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(rv32_handler.emit_events(ExecResult::ExecResultOK,
                                       ProposedTransaction{
                                           .signed_tx = rv32_handler_tx})());

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

TEST_F(Rv32RuntimeContextTest, DispatchesNonzeroMethodInGuest)
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

TEST_F(Rv32RuntimeContextTest, UnknownMethodIsMeteredGuestTrap)
{
  // v4 Option A: dispatch is in-guest. An unknown method id is not a
  // pre-execution host miss on the RV32 path: the runtime builds the Machine,
  // seeds the id, enters _start, and the guest dispatcher's default arm traps.
  // The miss is metered because the guest ran.
  constexpr Runtime::MethodID kUnknownMethodId = 2;
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto rv32_handler = make_new_system_call_handler(
      make_root_transaction(kEnoughGas, kUnknownMethodId), rv32_context);
  rv32_handler.enable_tracing();

  auto rv32_result = rv32_handler.root_invoke();

  ASSERT_FALSE(rv32_result.is_root_success());
  EXPECT_EQ(rv32_result.result_.error().code(), InvokeErrorCode::Deterministic);
  EXPECT_GT(rv32_result.gas_consumed_, 0U); // guest dispatcher ran (metered)
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
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
  // Three argument loads (a0..a2) plus the ecall instruction.
  EXPECT_EQ(result.gas_consumed_, 4U * rv32::kBaseInstructionGas);
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

  auto expect_parity = [&](HostFn0 wasm_entry, HostFn8 rv32_entry,
                           InvokeErrorCode expected) {
    for (const WasmEngine engine : kDifferentialWasmEngines) {
      auto rv32_context = rv32::RuntimeContext(
          {{kRv32ContractId, make_single_ecall_image(rv32_entry)}});
      auto handler =
          make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                       rv32_context);

      const auto wasm_result =
          invoke_wasm_error_contract(engine, wasm_entry, kCall1, kEnoughGas);
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
        invoke_wasm_error_contract(engine, &wasm_good_call_entry, kUnreachable,
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
                                   kCall1, kGasLimit);
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

  auto expect_mapping = [&](HostFn8 entry, InvokeErrorCode expected) {
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

TEST_F(Rv32RuntimeContextTest, ExitPanicMapsToDeterministicRegardlessOfCode)
{
  // Parity with the wasm runtime and production Rv32Runtime: Exit is always a
  // deterministic failure; the trampoline result_code is not an exit status.
  auto expect_deterministic = [&](HostFn8 entry) {
    auto rv32_context = rv32::RuntimeContext(
        {{kRv32ContractId, make_single_ecall_image(entry)}});
    auto contract = make_rv32_contract_xdr();
    install_contract(contract);

    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);

    auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

    ASSERT_FALSE(result.is_root_success());
    EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
  };

  expect_deterministic(&exit_entry);
  expect_deterministic(&zero_code_exit_entry);
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
  expect_deterministic(make_unaligned_oob_access_image());
  expect_deterministic(make_breakpoint_image());
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
  auto contract = make_non_mock_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);
}

// memory_bytes == 0 is an explicitly allowed (degenerate) image: a zero-memory
// contract. initial_memory must then be empty, which the size check enforces.
TEST_F(Rv32RuntimeContextTest, VerifyAcceptsZeroMemoryBytesImage)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, rv32::RuntimeContractImage{
                             .program = make_program({addi(1, 0, 1)}),
                             .initial_memory = {},
                             .memory_bytes = 0,
                         }}});
  auto contract = make_rv32_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract), InvokeErrorCode::None);
}

// The other half of the bound: memory_bytes above kMaxMemoryBytes is rejected
// at validation rather than deferred to a per-invocation construction trap.
TEST_F(Rv32RuntimeContextTest, VerifyRejectsOversizedMemoryBytesImage)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, rv32::RuntimeContractImage{
                             .program = make_program({addi(1, 0, 1)}),
                             .initial_memory = {},
                             .memory_bytes = rv32::kMaxMemoryBytes + 1,
                         }}});
  auto contract = make_rv32_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);
}

// Metadata cross-check for SDK-lowered RV32 artifacts.
TEST_F(Rv32RuntimeContextTest,
       LoweredSdkArtifactFormatFingerprintMatchesRuntime)
{
  const auto metadata_path = get_lowered_sdk_smoke_metadata_path();
  if (metadata_path.empty()) {
    GTEST_SKIP() << "build the generated ITC smoke artifact metadata "
                    "or set RV32_SMOKE_METADATA=/path/to/contract.rv32.json";
  }

  std::ifstream input(metadata_path);
  ASSERT_TRUE(input.is_open()) << "cannot open " << metadata_path;
  const std::string metadata((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  ASSERT_FALSE(metadata.empty());

  auto parsed = json::parse(metadata);
  ASSERT_TRUE(parsed.has_value()) << "could not parse " << metadata_path
                                  << " as JSON: " << parsed.error().message;
  ASSERT_EQ(parsed->type(), json::Value::Type::kObject);

  const json::Value *fingerprint = parsed->find("artifact_format_fingerprint");
  ASSERT_NE(fingerprint, nullptr)
      << "metadata is missing artifact_format_fingerprint";
  const auto fingerprint_text = fingerprint->as_string();
  // Guard the access with an explicit has_value() branch: clang-tidy's
  // bugprone-unchecked-optional-access models `if (opt.has_value())` but not
  // gtest's ASSERT_TRUE, and it flags value() and operator* alike.
  if (fingerprint_text.has_value()) {
    EXPECT_EQ(*fingerprint_text, artifact_format_fingerprint_hex());
  }
  else {
    ADD_FAILURE() << "artifact_format_fingerprint must be a string";
  }
}

TEST_F(Rv32RuntimeContextTest, LoweredSdkArtifactRealCalldataRoundTrip)
{
  const auto artifact_path = get_lowered_sdk_smoke_artifact_path();
  if (artifact_path.empty()) {
    GTEST_SKIP() << "build the generated ITC smoke artifact "
                    "or set RV32_SMOKE_ARTIFACT=/path/to/contract.rv32a to "
                    "run this end-to-end calldata smoke test";
  }

  auto loaded = load_lowered_sdk_smoke_artifact(artifact_path);
  if (!loaded.has_value()) {
    ADD_FAILURE() << "failed to load lowered SDK smoke artifact";
    return;
  }
  LoadedRv32SmokeArtifact loaded_artifact = std::move(*loaded);

  // v4 dispatches in-guest; there is no host-side dispatch table to inspect.
  // The increment-then-get round trip below exercises dispatch end-to-end:
  // each call enters _start with its method id seeded, and the guest routes it.
  const Runtime::MethodID increment_method =
      rv32_smoke_counter_method_id(u8"increment");
  const Runtime::MethodID get_method = rv32_smoke_counter_method_id(u8"get");

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, std::move(loaded_artifact.image)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  constexpr uint64_t kIncrementAmount = 7;
  // Match the client ABI shape: call arguments are encoded as a tuple. A
  // one-field tuple is byte-identical to the scalar in CXS, but spelling the
  // tuple keeps this test from teaching the wrong pattern for multi-arg calls.
  auto increment_calldata = Serializer<SerializationFormat::kCxs>::serialize(
      std::tuple<uint64_t>{kIncrementAmount});
  if (!increment_calldata.has_value()) {
    ADD_FAILURE() << "failed to encode rv32 counter increment calldata";
    return;
  }
  Calldata increment_calldata_bytes = std::move(*increment_calldata);

  auto increment_handler = make_new_system_call_handler(
      make_root_transaction(kParityGas, increment_method,
                            std::move(increment_calldata_bytes), 1),
      rv32_context);
  increment_handler.enable_tracing();
  const auto increment_result = increment_handler.root_invoke();
  ASSERT_TRUE(increment_result.is_root_success())
      << "rv32 counter increment did not return cleanly";
  EXPECT_EQ(increment_handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Return);
  ASSERT_EQ(increment_handler.try_commit_transaction(),
            ExecResult::ExecResultOK);
  ASSERT_NE(increment_handler.get_trace(), nullptr);
  auto const *increment_trace = increment_handler.get_trace();
  ASSERT_EQ(increment_trace->storage_writes.size(), 1U);
  auto const &increment_write = increment_trace->storage_writes[0];
  EXPECT_EQ(increment_write.addrkey.address, kRv32ContractAddress);
  ASSERT_EQ(increment_write.delta.type(),
            StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
  EXPECT_EQ(from_xdr(increment_write.delta.nnint().set_value), 0);
  EXPECT_EQ(from_xdr(increment_write.delta.nnint().add_value),
            kIncrementAmount);

  // NNInt deltas are visible to a later root transaction after block commit.
  advance_block();

  auto get_handler =
      make_new_system_call_handler(make_root_transaction(kParityGas, get_method,
                                                         Calldata{}, 2),
                                   rv32_context);
  get_handler.enable_tracing();
  const auto get_result = get_handler.root_invoke();
  ASSERT_TRUE(get_result.is_root_success())
      << "rv32 counter get did not return cleanly after increment";
  const auto get_output = get_handler.get_most_recent_frame_output();
  ASSERT_EQ(get_output.status_code(), InvocationFrameStatusCode::Return);

  const auto &return_buffer = get_output.return_buffer();
  ASSERT_NE(get_handler.get_trace(), nullptr);
  EXPECT_TRUE(get_handler.get_trace()->storage_writes.empty());
  auto decoded = Serializer<SerializationFormat::kCxs>::deserialize<uint64_t>(
      std::as_bytes(std::span(return_buffer.data(), return_buffer.size())));
  if (!decoded.has_value()) {
    ADD_FAILURE() << "failed to decode rv32 counter get()";
    return;
  }
  EXPECT_EQ(*decoded, kIncrementAmount);
  ASSERT_EQ(get_handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Smoke test for SDK-lowered RV32 artifacts.
//
// By default this uses the generated artifact produced from the ITC
// rv32_smoke_counter fixture. Set RV32_SMOKE_ARTIFACT to override it with any
// `.rv32a` path produced by scripts/rv32_make_smoke_artifact.sh, which wraps
// the published rv32-lower binary. The test parses, validates, invokes the
// selected dispatch entry, and prints the observed outcome. It's intentionally
// informational — EXPECT, not ASSERT — so a failure here surfaces the actual
// VM/SDK gap rather than just printing a green/red ctest line.
TEST_F(Rv32RuntimeContextTest, LoweredSdkArtifactSmoke)
{
  const auto artifact_path = get_lowered_sdk_smoke_artifact_path();
  if (artifact_path.empty()) {
    GTEST_SKIP() << "build the generated ITC smoke artifact "
                    "or set RV32_SMOKE_ARTIFACT=/path/to/contract.rv32a to "
                    "run this end-to-end smoke test";
  }

  auto loaded = load_lowered_sdk_smoke_artifact(artifact_path);
  if (!loaded.has_value()) {
    ADD_FAILURE() << "failed to load lowered SDK smoke artifact";
    return;
  }
  LoadedRv32SmokeArtifact loaded_artifact = std::move(*loaded);
  std::cerr << "[smoke] loaded artifact: " << loaded_artifact.artifact_size
            << " bytes from " << loaded_artifact.path << "\n";
  std::cerr << "[smoke] parsed: code_bytes="
            << loaded_artifact.image.program->byte_size()
            << " memory_bytes=" << loaded_artifact.image.memory_bytes << "\n";

  // v4 dispatches in-guest, so the target is chosen by method id, not a table
  // entry. Default to the no-arg `get` (returns cleanly without calldata); set
  // RV32_SMOKE_METHOD_ID=0x<16-hex> to target a specific method.
  uint64_t smoke_method_id = rv32_smoke_counter_method_id(u8"get");
  if (const char *want = std::getenv("RV32_SMOKE_METHOD_ID")) {
    smoke_method_id = std::strtoull(want, nullptr, 0);
  }
  std::cerr << "[smoke] invoking method_id=0x" << std::hex << smoke_method_id
            << std::dec << "\n";

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, std::move(loaded_artifact.image)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(
      make_root_transaction(kParityGas, smoke_method_id), rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  const auto result = handler.root_invoke();

  if (handler.get_trace() != nullptr) {
    std::cerr << "[smoke] trace status_code="
              << static_cast<int>(handler.get_trace()->status.status_code())
              << " syscalls=" << handler.get_trace()->system_calls.size()
              << "\n";
  }
  // Read the buffer that matches the frame's discriminant. The frame output
  // is an XDR union keyed on status_code, so accessing return_buffer() on an
  // Abort frame (or vice versa) throws — branch on the discriminant first.
  const auto frame_output = handler.get_most_recent_frame_output();
  const auto frame_status = frame_output.status_code();
  std::cerr << "[smoke] frame status_code=" << static_cast<int>(frame_status);
  switch (frame_status) {
  case InvocationFrameStatusCode::Return:
  {
    const auto &return_buffer = frame_output.return_buffer();
    static constexpr char kHex[] = "0123456789abcdef";
    std::cerr << " (Return) return_bytes=" << return_buffer.size() << " hex=";
    for (const auto byte : return_buffer) {
      std::cerr << kHex[(byte >> 4) & 0xf] << kHex[byte & 0xf];
    }
    break;
  }
  case InvocationFrameStatusCode::Abort:
    std::cerr << " (Abort) abort_bytes=" << frame_output.abort_buffer().size();
    break;
  case InvocationFrameStatusCode::Fault:
    std::cerr << " (Fault) no buffer";
    break;
  }
  std::cerr << "\n";
  if (handler.get_transaction_stats() != nullptr) {
    std::cerr << "[smoke] stats invalid_syscall_count="
              << handler.get_transaction_stats()->invalid_syscall_count << "\n";
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
