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

#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/json.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
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
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

// Test callback fixtures intentionally mirror C host function ABIs, and the
// test fixture combines gtest with the existing native-contract harness.
// NOLINTBEGIN(readability-named-parameter,misc-multiple-inheritance)

namespace hogd {
namespace {

namespace compiled_sha256_return = rv32::fixtures::compiled_sha256_return;

constexpr int32_t kRv32ContractId = 7;
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
// For the tests that read storage: a first touch of a key costs kDBRPCCost on
// top of the access, which is several times kParityGas on its own. Separate so
// that raising it does not move the traces the parity tests pin.
constexpr GasAmount kStorageReadGas = 200'000;

constexpr std::string_view kEffectStorageKey = "rv32_parity_key";
constexpr std::string_view kEffectStorageValue = "rv32 parity bytes";
constexpr std::string_view kEffectEventData = "rv32 parity event";
// The storage flags image writes the GetStorageFlags outputs here, one
// StorageDeltaFlagSet after the other, and returns them.
constexpr uint32_t kFlagsOutputAddress = 512;
constexpr std::string_view kFlagsStorageKey = "rv32_flags_key";
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
format_fingerprint_hex(uint64_t fingerprint)
{
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0') << std::setw(16) << fingerprint;
  return out.str();
}

// Every fingerprint a lowerer may report, current one first. More than one only
// while kAcceptedGasSchedules retains previous schedules.
std::vector<std::string>
accepted_format_fingerprints_hex()
{
  std::vector<std::string> accepted;
  accepted.reserve(rv32::kAcceptedFormatFingerprints.size());
  for (const uint64_t fingerprint : rv32::kAcceptedFormatFingerprints) {
    accepted.push_back(format_fingerprint_hex(fingerprint));
  }
  return accepted;
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

  auto parsed =
      rv32::parse_artifact(artifact_bytes, rv32::Rv32Limits::defaults());
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

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
make_host_panic_result(HostFnErrorCode code, uint64_t gas_consumed = 4)
{
  return {HostFnError{code}, gas_consumed};
}

MaybeAsyncSyscallResult
unrecoverable_entry(ExternHostCallContext /*unused*/,
                    ExecutionContext /*unused*/, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Unrecoverable);
}

MaybeAsyncSyscallResult
validation_failure_entry(ExternHostCallContext /*unused*/,
                         ExecutionContext /*unused*/, uint32_t, uint32_t,
                         uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::ValidationFailure);
}

MaybeAsyncSyscallResult
abandon_entry(ExternHostCallContext /*unused*/, ExecutionContext /*unused*/,
              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
              uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Abandon);
}

MaybeAsyncSyscallResult
throwing_entry(ExternHostCallContext /*unused*/, ExecutionContext /*unused*/,
               uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
               uint32_t, uint32_t)
{
  throw 1;
}

MaybeAsyncSyscallResult
host_out_of_gas_entry(ExternHostCallContext /*unused*/,
                      ExecutionContext /*unused*/, uint32_t, uint32_t, uint32_t,
                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::OutOfGas, 0);
}

MaybeAsyncSyscallResult
exit_entry(ExternHostCallContext /*unused*/, ExecutionContext /*unused*/,
           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
           uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Exit, 0);
}

MaybeAsyncSyscallResult
return_entry(ExternHostCallContext /*unused*/, ExecutionContext /*unused*/,
             uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
             uint32_t, uint32_t)
{
  return make_host_panic_result(HostFnErrorCode::Return);
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
make_single_ecall_image(SyscallFunction entry)
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
  auto artifact = rv32::parse_artifact(compiled_sha256_return::kArtifact,
                                       rv32::Rv32Limits::defaults());
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
  auto parsed = rv32::parse_artifact(artifact, rv32::Rv32Limits::defaults());
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
  // BytestringSet(key, value, value_len).
  append_syscall(program, {static_cast<int32_t>(SyscallNo::kBytestringSet),
                           static_cast<int32_t>(kEffectStorageKeyAddress),
                           static_cast<int32_t>(kEffectStorageValueAddress),
                           static_cast<int32_t>(kEffectStorageValue.size())});
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

// Method 1 writes a key, then drives the storage flags syscalls on it through
// the register ABI: set the lock, read it, clear it, read it again, and set it
// once more so the commit carries it. It returns the two GetStorageFlags
// outputs. Any other method writes the same value with no flags and returns
// nothing.
rv32::RuntimeContractImage
make_storage_flags_image()
{
  std::vector<uint8_t> initial_memory(kEffectMemoryBytes);
  const StorageKeyContractData key = NativeContractTestFramework::
      make_storage_key_matching_static_key_sdk_macro(kFlagsStorageKey);
  std::ranges::copy(key, initial_memory.begin() + kEffectStorageKeyAddress);
  std::ranges::copy(kEffectStorageValue,
                    initial_memory.begin() + kEffectStorageValueAddress);

  const auto key_address = static_cast<int32_t>(kEffectStorageKeyAddress);
  const auto exclusive = static_cast<int32_t>(StorageDeltaFlags::ExclusiveLock);
  const auto first_output = static_cast<int32_t>(kFlagsOutputAddress);
  const auto second_output =
      static_cast<int32_t>(kFlagsOutputAddress + sizeof(StorageDeltaFlagSet));
  // BytestringSet(key, value, value_len).
  auto append_write = [&](std::vector<uint32_t> &program) {
    append_syscall(program, {static_cast<int32_t>(SyscallNo::kBytestringSet),
                             key_address,
                             static_cast<int32_t>(kEffectStorageValueAddress),
                             static_cast<int32_t>(kEffectStorageValue.size())});
  };

  std::vector<uint32_t> flags_method;
  append_write(flags_method);
  // SetStorageFlags(key, flags).
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kSetStorageFlags),
                  key_address, exclusive});
  // GetStorageFlags(key, flags_out).
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kGetStorageFlags),
                  key_address, first_output});
  // ClearStorageFlags(key).
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kClearStorageFlags),
                  key_address});
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kGetStorageFlags),
                  key_address, second_output});
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kSetStorageFlags),
                  key_address, exclusive});
  // Return(buffer, buffer_len, ...) with both outputs. The remaining Return
  // registers are loaded with zero, since earlier calls left values in them.
  append_syscall(flags_method,
                 {static_cast<int32_t>(SyscallNo::kReturn), first_output,
                  static_cast<int32_t>(2 * sizeof(StorageDeltaFlagSet)), 0, 0,
                  0});

  // x6 = 1; if a0 (method id low) != 1, branch past the flags method to the
  // plain write.
  const auto plain_write_offset =
      static_cast<int32_t>((flags_method.size() + 1) * rv32::kInstructionBytes);
  std::vector<uint32_t> program = {
      addi(6, 0, 1),
      rv32::test_program::branch(1, 10, 6, plain_write_offset),
  };
  program.insert(program.end(), flags_method.begin(), flags_method.end());
  append_write(program);
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
  auto runtime = rv32_context.make_new_runtime(deployed, &handler)();
  EXPECT_TRUE(runtime.has_value());
  EXPECT_NE(runtime.value(), nullptr);
  if (!runtime.has_value() || runtime.value() == nullptr) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::Unrecoverable},
                        .gas_consumed_ = 0};
  }
  return runtime.value()->invoke(method_id, gas_limit)();
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
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  void install_contract(std::shared_ptr<const Contract> const &contract)
  {
    write_contract(contract);
    write_deployment(kRv32ContractAddress, contract);
    setup_grant_storage_token(kRv32ContractAddress, 1'000'000'000);
  }
};

// Exact-pin helpers mirroring tracing/trace_shape_tests.cc: expected traces
// are hardcoded SyscallTrace values, not comparisons against another runtime
// or a second run. Absolute gas values shift when the gas schedule or the
// hand-assembled/compiled fixtures change; regenerate them deliberately (the
// failure message prints the observed trace).
using SyscallTraceVec = x23::Vec<SyscallTrace, 0xffffffff>;

HostFunctionStatusXdr
ok_status()
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = SyscallErrorCode::kSyscallErrorCodeNone;
  return status;
}

HostFunctionStatusXdr
error_status(SyscallErrorCode code)
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = code;
  return status;
}

SyscallTrace
make_trace_entry(GasAmount gas_at_start, GasAmount gas_at_end,
                 SyscallTraceBody body, HostFunctionStatusXdr status)
{
  return SyscallTrace{
      .gas_remaining_at_start = gas_at_start,
      .gas_remaining_at_end = gas_at_end,
      .body = std::move(body),
      .status = std::move(status),
  };
}

SyscallTrace
return_trace_entry(GasAmount gas, std::span<const uint8_t> buffer,
                   bool revert_frame = false)
{
  SyscallTraceBody body(SyscallNo::kReturn);
  body.return_trace().inputs.buffer.append_range(buffer);
  body.return_trace().inputs.revert_frame = revert_frame;
  return make_trace_entry(gas, gas - kSyscallBaseCost, std::move(body),
                          HostFunctionStatusXdr(HostFnErrorCode::Return));
}

SyscallTrace
sha256_trace_entry(GasAmount gas_at_start, std::span<const uint8_t> input,
                   SHA256Hash const &hash)
{
  SyscallTraceBody body(SyscallNo::kSHA256);
  body.sha256().inputs.input.append_range(input);
  body.sha256().outputs = SyscallSHA256Outputs{.hash = hash};
  return make_trace_entry(gas_at_start,
                          gas_at_start - GasCost::sha256_hash(input.size()) -
                              kSyscallBaseCost,
                          std::move(body), ok_status());
}

TEST_F(Rv32RuntimeContextTest, InvalidSyscallThenReturnTraceIsExact)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_invalid_syscall_then_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  const auto result = handler.root_invoke()();
  ASSERT_TRUE(result.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(output.return_buffer().empty());

  // 1 addi + ecall for the out-of-range syscall number (traced under its raw
  // discriminant, charged kNoSyscallCost, and continues), then 6 addi + ecall
  // for the empty Return.
  SyscallTraceVec expected;
  expected.push_back(make_trace_entry(
      kEnoughGas - (2 * rv32::kBaseInstructionGas),
      kEnoughGas - (2 * rv32::kBaseInstructionGas) - kNoSyscallCost -
          kSyscallBaseCost,
      SyscallTraceBody(static_cast<SyscallNo>(kSyscallCount)),
      error_status(SyscallErrorCode::kSyscallErrorCodeNoSyscall)));
  expected.push_back(return_trace_entry(kEnoughGas -
                                            (9 * rv32::kBaseInstructionGas) -
                                            kNoSyscallCost - kSyscallBaseCost,
                                        {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_EQ(handler.get_trace()->status.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_TRUE(handler.get_trace()->status.return_buffer().empty());
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
  EXPECT_TRUE(handler.get_most_recent_frame_events().empty());

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 1);
}

TEST_F(Rv32RuntimeContextTest, StorageWriteAndEventTraceIsExact)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_storage_event_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  const SignedTransaction tx{.tx = make_root_transaction(kParityGas)};
  auto handler = make_new_system_call_handler(tx.tx, rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  const auto result = handler.root_invoke()();
  ASSERT_TRUE(result.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(output.return_buffer().empty());

  ASSERT_NE(handler.get_trace(), nullptr);
  auto const &trace = *handler.get_trace();
  const auto events = handler.get_most_recent_frame_events();

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                  ProposedTransaction{.signed_tx = tx})());

  const StorageKeyContractData key =
      make_storage_key_matching_static_key_sdk_macro(kEffectStorageKey);
  const std::vector<uint8_t> value(kEffectStorageValue.begin(),
                                   kEffectStorageValue.end());
  Topic topic0;
  topic0.fill(0x5a);
  const std::vector<uint8_t> data(kEffectEventData.begin(),
                                  kEffectEventData.end());

  // BytestringSet is 4 addi + ecall, LogEvent 6 addi + ecall, and the empty
  // Return 6 addi + ecall; syscall costs beyond base instruction gas come
  // from the gas cost table.
  //
  // The write pays its own cost on top of the crossing -- the storage access
  // plus one block for these bytes -- and every later entry starts that much
  // lower. Named rather than folded into the literals so the next repricing
  // moves these expectations with the schedule instead of through them.
  const GasAmount write_cost = GasCost::bytestring_set(value.size());
  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    expected.push_back(
        make_trace_entry(kParityGas - 5,
                         kParityGas - 5 - kSyscallBaseCost - write_cost,
                         std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kLogEvent);
    body.log_event().inputs.topic0 = topic0;
    body.log_event().inputs.data.append_range(data);
    expected.push_back(
        make_trace_entry(kParityGas - 12 - kSyscallBaseCost - write_cost,
                         kParityGas - 12 - (2 * kSyscallBaseCost) - write_cost,
                         std::move(body), ok_status()));
  }
  expected.push_back(
      return_trace_entry(kParityGas - 19 - (2 * kSyscallBaseCost) - write_cost,
                         {}));

  EXPECT_EQ(trace.status.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(trace.status.return_buffer().empty());
  ASSERT_EQ(trace.system_calls, expected) << x23::show(trace.system_calls);

  // The committed storage delta and the emitted event mirror the trace.
  ASSERT_EQ(trace.storage_writes.size(), 1U);
  auto const &write = trace.storage_writes[0];
  EXPECT_EQ(write.addrkey,
            make_contract_data_addrkey_matching_static_key_sdk_macro(
                kRv32ContractAddress, kEffectStorageKey));
  EXPECT_EQ(write.flags, 0U);
  ASSERT_EQ(write.delta.type(), StorageDeltaType::kDeltaTypeBytestringSet);
  EXPECT_TRUE(std::ranges::equal(write.delta.bytestring(), value));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].contract_address, kRv32ContractAddress);
  EXPECT_EQ(events[0].num_topics, 1U);
  x23::Vec<uint8_t, kMaxEventLength> expected_event;
  expected_event.append_range(topic0);
  expected_event.append_range(data);
  EXPECT_EQ(events[0].serialized_event, expected_event);

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
}

// The storage flags syscalls through the guest register ABI: their arguments
// as traced, GetStorageFlags' output as written to guest memory, and the lock
// the commit carries, which keeps a second writer out for the block.
TEST_F(Rv32RuntimeContextTest, StorageFlagsTraceIsExact)
{
  constexpr Runtime::MethodID kFlagsMethod = 1;
  constexpr Runtime::MethodID kPlainWriteMethod = 0;
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, make_storage_flags_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  auto handler = make_new_system_call_handler(
      make_root_transaction(kParityGas, kFlagsMethod), rv32_context);
  handler.enable_tracing();

  const auto result = handler.root_invoke()();
  ASSERT_TRUE(result.is_root_success());

  constexpr StorageDeltaFlagSet kExclusive =
      std::to_underlying(StorageDeltaFlags::ExclusiveLock);
  constexpr StorageDeltaFlagSet kNone =
      std::to_underlying(StorageDeltaFlags::None);
  // GetStorageFlags stores each StorageDeltaFlagSet little-endian.
  const std::array<uint8_t, 2 * sizeof(StorageDeltaFlagSet)> flags_out = {
      static_cast<uint8_t>(kExclusive), 0, 0, 0, 0, 0, 0, 0};

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), flags_out));

  ASSERT_NE(handler.get_trace(), nullptr);
  auto const &trace = *handler.get_trace();

  const StorageKeyContractData key =
      make_storage_key_matching_static_key_sdk_macro(kFlagsStorageKey);
  const std::vector<uint8_t> value(kEffectStorageValue.begin(),
                                   kEffectStorageValue.end());

  // The dispatch runs an addi and a not-taken branch. Each call then runs one
  // addi per loaded register and the ecall, and pays the crossing and its own
  // cost: the flags calls each pay the storage access.
  SyscallTraceVec expected;
  GasAmount gas =
      kParityGas - rv32::kBaseInstructionGas - rv32::kBranchInstructionGas;
  auto expect_call = [&](uint64_t registers, GasAmount cost,
                         SyscallTraceBody body) {
    gas -= (registers + 1) * rv32::kBaseInstructionGas;
    const GasAmount gas_at_start = gas;
    gas -= kSyscallBaseCost + cost;
    expected.push_back(
        make_trace_entry(gas_at_start, gas, std::move(body), ok_status()));
  };
  auto set_flags_body = [&] {
    SyscallTraceBody body(SyscallNo::kSetStorageFlags);
    body.set_storage_flags().inputs.key = key;
    body.set_storage_flags().inputs.flags = kExclusive;
    return body;
  };
  auto get_flags_body = [&](StorageDeltaFlagSet flags) {
    SyscallTraceBody body(SyscallNo::kGetStorageFlags);
    body.get_storage_flags().inputs.key = key;
    body.get_storage_flags().outputs.emplace(
        SyscallGetStorageFlagsOutputs{.flags = flags});
    return body;
  };
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    expect_call(4, GasCost::bytestring_set(value.size()), std::move(body));
  }
  expect_call(3, kStorageAccessCost, set_flags_body());
  expect_call(3, kStorageAccessCost, get_flags_body(kExclusive));
  {
    SyscallTraceBody body(SyscallNo::kClearStorageFlags);
    body.clear_storage_flags().inputs.key = key;
    expect_call(2, kStorageAccessCost, std::move(body));
  }
  expect_call(3, kStorageAccessCost, get_flags_body(kNone));
  expect_call(3, kStorageAccessCost, set_flags_body());
  gas -= 7 * rv32::kBaseInstructionGas;
  expected.push_back(return_trace_entry(gas, flags_out));

  EXPECT_EQ(trace.status.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(trace.system_calls, expected) << x23::show(trace.system_calls);

  ASSERT_EQ(trace.storage_writes.size(), 1U);
  auto const &write = trace.storage_writes[0];
  EXPECT_EQ(write.addrkey,
            make_contract_data_addrkey_matching_static_key_sdk_macro(
                kRv32ContractAddress, kFlagsStorageKey));
  EXPECT_EQ(write.flags, kExclusive);
  ASSERT_EQ(write.delta.type(), StorageDeltaType::kDeltaTypeBytestringSet);
  EXPECT_TRUE(std::ranges::equal(write.delta.bytestring(), value));

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  // The committed lock holds the key for the rest of the block: a second
  // transaction writing the same value without a lock, which HogDb would
  // otherwise admit, is refused.
  auto second = make_new_system_call_handler(
      make_root_transaction(kParityGas, kPlainWriteMethod, Calldata{}, 2),
      rv32_context);
  ASSERT_TRUE(second.root_invoke()().is_root_success());
  EXPECT_EQ(second.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError);
}

TEST_F(Rv32RuntimeContextTest, Sha256ThenReturnTraceIsExact)
{
  const x23::Vec<uint8_t, 0xffffffff> input = {3, 1, 4, 1, 5, 9};
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_sha256_then_return_image(input)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kParityGas),
                                              rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  const auto result = handler.root_invoke()();

  ASSERT_TRUE(result.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), expected_hash));

  // SHA256 is 4 addi + ecall; the hash-returning Return is 6 addi + ecall.
  const GasAmount sha_gas = kParityGas - (5 * rv32::kBaseInstructionGas);
  const GasAmount return_gas = sha_gas - GasCost::sha256_hash(input.size()) -
                               kSyscallBaseCost -
                               (7 * rv32::kBaseInstructionGas);
  SyscallTraceVec expected;
  expected.push_back(sha256_trace_entry(sha_gas, input, expected_hash));
  expected.push_back(return_trace_entry(return_gas, expected_hash));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
  // return_gas is where the Return syscall starts; the frame ends one
  // crossing below that, which is what the Return itself costs.
  EXPECT_EQ(result.gas_consumed_, kParityGas - (return_gas - kSyscallBaseCost));

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
            input.size());
}

TEST_F(Rv32RuntimeContextTest, CompiledSha256ReturnTraceIsExact)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256MethodId),
      rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  const auto result = handler.root_invoke()();

  ASSERT_TRUE(result.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), expected_hash));

  // Absolute gas pins for the compiled fixture; they shift when the fixture
  // is recompiled or the gas schedule changes.
  const GasAmount sha_gas = 9'977;
  const GasAmount return_gas = 9'898;
  SyscallTraceVec expected;
  expected.push_back(sha256_trace_entry(sha_gas, input, expected_hash));
  expected.push_back(return_trace_entry(return_gas, expected_hash));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
  // return_gas is where the Return syscall starts; the frame ends one
  // crossing below that, which is what the Return itself costs.
  EXPECT_EQ(result.gas_consumed_, kParityGas - (return_gas - kSyscallBaseCost));

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
            input.size());
}

TEST_F(Rv32RuntimeContextTest, CompiledMultiMethodDispatchTraceIsExact)
{
  const x23::Vec<uint8_t, 0xffffffff> input(
      compiled_sha256_return::kInput.begin(),
      compiled_sha256_return::kInput.end());
  const SHA256Hash expected_hash = sha256(input);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_compiled_sha256_then_return_image()}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(
      make_root_transaction(kParityGas,
                            compiled_sha256_return::kSha256RepeatedMethodId),
      rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  const auto result = handler.root_invoke()();

  ASSERT_TRUE(result.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(output.return_buffer().empty());

  // The repeated-hash method hashes once and returns an empty buffer.
  // Absolute gas pins for the compiled fixture; they shift when the fixture
  // is recompiled or the gas schedule changes.
  const GasAmount sha_gas = 9'974;
  const GasAmount return_gas = 9'895;
  SyscallTraceVec expected;
  expected.push_back(sha256_trace_entry(sha_gas, input, expected_hash));
  expected.push_back(return_trace_entry(return_gas, {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
  // return_gas is where the Return syscall starts; the frame ends one
  // crossing below that, which is what the Return itself costs.
  EXPECT_EQ(result.gas_consumed_, kParityGas - (return_gas - kSyscallBaseCost));

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
            input.size());
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

  const auto result1 = handler1.root_invoke()();
  const auto result2 = handler2.root_invoke()();

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

  const auto result1 = handler1.root_invoke()();
  const auto result2 = handler2.root_invoke()();

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

  auto result = handler.root_invoke()();

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

  auto rv32_result = rv32_handler.root_invoke()();

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
  auto result = handler.root_invoke()();

  ASSERT_TRUE(result.is_root_success());
  // Three argument loads (a0..a2) plus the ecall instruction.
  EXPECT_EQ(result.gas_consumed_,
            (4U * rv32::kBaseInstructionGas) + kSyscallBaseCost);
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

TEST_F(Rv32RuntimeContextTest, HostTrapPanicCodesMapToInvokeOutcomes)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_mapping = [&](SyscallFunction entry, InvokeErrorCode expected) {
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
  // Parity with production Rv32Runtime: Exit is always a deterministic
  // failure; the trampoline result_code is not an exit status.
  auto expect_deterministic = [&](SyscallFunction entry) {
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
}

TEST_F(Rv32RuntimeContextTest, GuestTrapsMapToDeterministic)
{
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto expect_deterministic = [&](rv32::RuntimeContractImage image,
                                  GasAmount expected_gas) {
    auto rv32_context =
        rv32::RuntimeContext({{kRv32ContractId, std::move(image)}});
    auto handler =
        make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                     rv32_context);
    auto result = direct_invoke(contract, rv32_context, handler, kEnoughGas);

    ASSERT_FALSE(result.is_root_success());
    EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
    EXPECT_EQ(result.gas_consumed_, expected_gas);
  };

  // Instructions retired before the trap are metered; the trapping fetch or
  // access itself is charged too.
  expect_deterministic(make_bad_pc_image(), 1 * rv32::kBaseInstructionGas);
  // Both bad-memory images fault on an lw, which is priced as a wide load.
  expect_deterministic(make_bad_memory_image(), rv32::kWideLoadGas);
  expect_deterministic(make_unaligned_oob_access_image(),
                       rv32::kBaseInstructionGas + rv32::kWideLoadGas);
  expect_deterministic(make_breakpoint_image(), 1 * rv32::kBaseInstructionGas);
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

  EXPECT_EQ(rv32_context.verify_contract(*contract, NetworkConfigParams{}),
            InvokeErrorCode::Deterministic);
}

TEST_F(Rv32RuntimeContextTest, VerifyRejectsNonMockContractWithoutAbort)
{
  auto rv32_context = rv32::RuntimeContext({});
  auto contract = make_non_mock_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract, NetworkConfigParams{}),
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

  EXPECT_EQ(rv32_context.verify_contract(*contract, NetworkConfigParams{}),
            InvokeErrorCode::None);
}

// The other half of the bound: memory_bytes above the VM's ceiling is
// rejected at validation rather than deferred to a per-invocation construction
// trap.
TEST_F(Rv32RuntimeContextTest, VerifyRejectsOversizedMemoryBytesImage)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, rv32::RuntimeContractImage{
                             .program = make_program({addi(1, 0, 1)}),
                             .initial_memory = {},
                             .memory_bytes = rv32::kMemoryCeilingBytes + 1,
                         }}});
  auto contract = make_rv32_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract, NetworkConfigParams{}),
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
    // Accept any fingerprint in the transitional window, not just this build's:
    // the lowerer that stamps the current gas schedule may not have rolled out
    // yet, and its fingerprint folds that schedule and artifact format. Keep
    // v5 producers accepted while the runtime supports v5 artifacts.
    const auto accepted = accepted_format_fingerprints_hex();
    EXPECT_TRUE(std::ranges::contains(accepted, *fingerprint_text))
        << "lowered metadata reports " << *fingerprint_text
        << ", which is not an accepted engine fingerprint; this build's is "
        << format_fingerprint_hex(rv32::kArtifactFormatFingerprint);
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
      make_root_transaction(kStorageReadGas, increment_method,
                            std::move(increment_calldata_bytes), 1),
      rv32_context);
  increment_handler.enable_tracing();
  const auto increment_result = increment_handler.root_invoke()();
  ASSERT_TRUE(increment_result.is_root_success())
      << "rv32 counter increment did not return cleanly";
  EXPECT_EQ(increment_handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Return);
  ASSERT_EQ(increment_handler.try_commit_transaction()(),
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

  auto get_handler = make_new_system_call_handler(
      make_root_transaction(kStorageReadGas, get_method, Calldata{}, 2),
      rv32_context);
  get_handler.enable_tracing();
  const auto get_result = get_handler.root_invoke()();
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
  ASSERT_EQ(get_handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Run the generated ITC rv32_smoke_counter artifact through the canonical test
// runtime. RV32_SMOKE_ARTIFACT overrides the path; RV32_SMOKE_METHOD_ID selects
// the guest-dispatched method (default: get). Success requires a clean return.
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
      make_root_transaction(kStorageReadGas, smoke_method_id), rv32_context);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  const auto result = handler.root_invoke()();

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

  EXPECT_TRUE(result.is_root_success())
      << "lowered artifact did not return cleanly — check trace status above";
}

TEST_F(Rv32RuntimeContextTest, DeleteObjectChargesStorageAccess)
{
  constexpr std::array<std::string_view, 3> kKeyNames = {"rv32_delete_key_0",
                                                         "rv32_delete_key_1",
                                                         "rv32_delete_key_2"};
  constexpr uint32_t kKeyBaseAddress = 256;
  constexpr uint32_t kMemoryBytes = 1024;

  std::vector<uint8_t> initial_memory(kMemoryBytes);
  std::vector<uint32_t> program;
  for (size_t i = 0; i < kKeyNames.size(); ++i) {
    const StorageKeyContractData key =
        make_storage_key_matching_static_key_sdk_macro(kKeyNames[i]);
    const uint32_t key_address =
        kKeyBaseAddress + (i * sizeof(StorageKeyContractData));
    std::ranges::copy(key, initial_memory.begin() + key_address);
    append_syscall(program, {static_cast<int32_t>(SyscallNo::kDeleteObject),
                             static_cast<int32_t>(key_address)});
  }
  append_syscall(program,
                 {static_cast<int32_t>(SyscallNo::kReturn), 0, 0, 0, 0, 0});

  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, rv32::RuntimeContractImage{
                             .program = make_program(program),
                             .initial_memory = std::move(initial_memory),
                             .memory_bytes = kMemoryBytes,
                         }}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kParityGas),
                                              rv32_context);
  handler.enable_tracing();

  const auto result = handler.root_invoke()();
  ASSERT_TRUE(result.is_root_success());
  ASSERT_NE(handler.get_trace(), nullptr);
  auto const &trace = *handler.get_trace();
  ASSERT_EQ(trace.status.status_code(), InvocationFrameStatusCode::Return);

  ASSERT_EQ(trace.system_calls.size(), kKeyNames.size() + 1);
  for (size_t i = 0; i < kKeyNames.size(); ++i) {
    auto const &delete_trace = trace.system_calls[i];
    EXPECT_EQ(delete_trace.body.syscall_number(), SyscallNo::kDeleteObject);
    // The delete walks the frame stack, creates the first-touch item view, and
    // installs the delete delta, so it pays the storage access on top of the
    // crossing, as the storage-flags and NNInt syscalls do.
    EXPECT_EQ(delete_trace.gas_remaining_at_start -
                  delete_trace.gas_remaining_at_end,
              kSyscallBaseCost + kStorageAccessCost)
        << "DeleteObject key " << i << " did not charge kStorageAccessCost";
  }

  ASSERT_EQ(trace.storage_writes.size(), kKeyNames.size());
  for (const std::string_view key_name : kKeyNames) {
    const AddressAndKey expected_addrkey =
        make_contract_data_addrkey_matching_static_key_sdk_macro(
            kRv32ContractAddress, key_name);
    const auto write =
        std::ranges::find_if(trace.storage_writes, [&](auto const &candidate) {
          return candidate.addrkey == expected_addrkey;
        });
    ASSERT_NE(write, trace.storage_writes.end());
    EXPECT_EQ(write->delta.type(), StorageDeltaType::kDeltaTypeDelete);
  }

  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

} // namespace
} // namespace hogd

// NOLINTEND(readability-named-parameter,misc-multiple-inheritance)
