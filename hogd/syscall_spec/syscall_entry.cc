#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

bool
check_memory_range(uint64_t memory_len, uint64_t base, uint64_t len)
{
  uint64_t base_end = 0;
  if (__builtin_add_overflow(base, len, &base_end))
    return false;
  return (base_end <= memory_len);
}

template<typename T> requires std::is_trivially_copyable_v<T>
T *
check_user_argument(std::span<std::byte> contract_memory, uint64_t user_offset,
                    uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return nullptr;
  }
  return reinterpret_cast<T *>(contract_memory.data() + user_offset);
}

std::optional<std::span<std::byte>>
check_user_span_argument(std::span<std::byte> contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return std::nullopt;
  }
  return contract_memory.subspan(user_offset, user_len);
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_outer(SystemCallHandler &syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                     uint64_t user_output_hash, uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  SHA256Hash *output_hash =
      check_user_argument<SHA256Hash>(contract_memory, user_output_hash,
                                      sizeof(SHA256Hash));

  if (!output_hash) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_SHA256_inner(*input_buffer, *output_hash);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetMsgSender_outer(SystemCallHandler &syscall_handler,
                           std::span<std::byte> contract_memory,
                           uint64_t user_output_sender, uint64_t /*unused*/,
                           uint64_t /*unused*/, uint64_t /*unused*/)
{
  ContractAddress *output_sender =
      check_user_argument<ContractAddress>(contract_memory, user_output_sender,
                                           sizeof(ContractAddress));

  if (!output_sender) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_GetMsgSender_inner(syscall_handler, *output_sender);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGetLen_outer(SystemCallHandler &syscall_handler,
                               std::span<std::byte> contract_memory,
                               uint64_t user_input_key,
                               uint64_t user_output_length, uint64_t /*unused*/,
                               uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_length =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_length,
                                               sizeof(RuntimeMemoryLength));

  if (!output_length) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_BytestringGetLen_inner(syscall_handler, *input_key,
                                        *output_length);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGet_outer(SystemCallHandler &syscall_handler,
                            std::span<std::byte> contract_memory,
                            uint64_t user_input_key,
                            uint64_t user_output_bytestring,
                            uint64_t user_output_bytestring_len,
                            uint64_t user_output_written_len)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  std::optional<std::span<std::byte>> output_bytestring =
      check_user_span_argument(contract_memory, user_output_bytestring,
                               user_output_bytestring_len);

  if (!output_bytestring) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_written_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_written_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_written_len) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_BytestringGet_inner(syscall_handler, *input_key,
                                     *output_bytestring, *output_written_len);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringSet_outer(SystemCallHandler &syscall_handler,
                            std::span<std::byte> contract_memory,
                            uint64_t user_input_key,
                            uint64_t user_input_bytestring,
                            uint64_t user_input_bytestring_len,
                            uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  std::optional<std::span<const std::byte>> input_bytestring =
      check_user_span_argument(contract_memory, user_input_bytestring,
                               user_input_bytestring_len);

  if (!input_bytestring) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_BytestringSet_inner(syscall_handler, *input_key,
                                     *input_bytestring);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_HasKey_outer(SystemCallHandler &syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t user_input_key, uint64_t user_output_status,
                     uint64_t /*unused*/, uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  StorageObjectType *output_status =
      check_user_argument<StorageObjectType>(contract_memory,
                                             user_output_status,
                                             sizeof(StorageObjectType));

  if (!output_status) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_HasKey_inner(syscall_handler, *input_key, *output_status);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeleteLast_outer(SystemCallHandler &syscall_handler,
                         std::span<std::byte> contract_memory,
                         uint64_t user_input_key, uint64_t /*unused*/,
                         uint64_t /*unused*/, uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_DeleteLast_inner(syscall_handler, *input_key);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldataLen_outer(SystemCallHandler &syscall_handler,
                             std::span<std::byte> contract_memory,
                             uint64_t user_output_len, uint64_t /*unused*/,
                             uint64_t /*unused*/, uint64_t /*unused*/)
{
  RuntimeMemoryLength *output_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory, user_output_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_len) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_GetCalldataLen_inner(syscall_handler, *output_len);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldata_outer(SystemCallHandler &syscall_handler,
                          std::span<std::byte> contract_memory,
                          uint64_t user_output_buffer,
                          uint64_t user_output_buffer_len,
                          uint64_t user_output_buffer_written_len,
                          uint64_t /*unused*/)
{
  std::optional<std::span<std::byte>> output_buffer =
      check_user_span_argument(contract_memory, user_output_buffer,
                               user_output_buffer_len);

  if (!output_buffer) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_buffer_written_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_buffer_written_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_buffer_written_len) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_GetCalldata_inner(syscall_handler, *output_buffer,
                                   *output_buffer_written_len);
}
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Log_outer(SystemCallHandler &syscall_handler,
                  std::span<std::byte> contract_memory,
                  uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                  uint64_t /*unused*/, uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {SyscallErrorCode::kFault, kGenericBadSyscallCost};
  }

  return syscall_Log_inner(syscall_handler, *input_buffer);
}

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> (*)(
    SystemCallHandler &, std::span<std::byte>, uint64_t, uint64_t, uint64_t,
    uint64_t);

const std::array<std::pair<SyscallNo, SyscallFunc>, 10> syscall_table = {
    std::pair{SyscallNo::kSHA256, &syscall_SHA256_outer},
    std::pair{SyscallNo::kGetMsgSender, &syscall_GetMsgSender_outer},
    std::pair{SyscallNo::kBytestringGetLen, &syscall_BytestringGetLen_outer},
    std::pair{SyscallNo::kBytestringGet, &syscall_BytestringGet_outer},
    std::pair{SyscallNo::kBytestringSet, &syscall_BytestringSet_outer},
    std::pair{SyscallNo::kHasKey, &syscall_HasKey_outer},
    std::pair{SyscallNo::kDeleteLast, &syscall_DeleteLast_outer},
    std::pair{SyscallNo::kGetCalldataLen, &syscall_GetCalldataLen_outer},
    std::pair{SyscallNo::kGetCalldata, &syscall_GetCalldata_outer},
    std::pair{SyscallNo::kLog, &syscall_Log_outer},
};

// returns gas_consumed, syscall error>
std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  if (arg0 >= syscall_table.size()) {
    // invalid syscall
    return {SyscallErrorCode::kUnknownSyscall, kUnknownSyscallCost};
  }

  static_assert(syscall_table.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = syscall_table[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  assert(call_context->syscall_handler);

  return syscall_fn_outer(*call_context->syscall_handler, contract_memory, arg1,
                          arg2, arg3, arg4);
}

HostFnStatus<int32_t>
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  auto [err, gas_consumed] =
      syscall_entry_inner(call_context, arg0, arg1, arg2, arg3, arg4);

  if (!call_context->runtime->consume_gas(gas_consumed)) {
    return HostFnError{HostFnErrorCode::kOutOfGas};
  }

  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "mismatch");

  return err.and_then([](SyscallErrorCode err) -> HostFnStatus<int32_t> {
    return static_cast<int32_t>(err);
  });
}

} // namespace hogd
