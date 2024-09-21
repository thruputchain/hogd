#include "hogd/hogexec/runtime/syscall_entry.h"

#include <cstring>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
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

std::pair<uint64_t, SyscallErrorCode>
syscall_SHA256_outer(std::span<std::byte> contract_memory,
                     uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                     uint64_t user_output_hash, uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  SHA256Hash *output_hash =
      check_user_argument<SHA256Hash>(contract_memory, user_output_hash,
                                      sizeof(SHA256Hash));

  if (!output_hash) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_SHA256_inner(*input_buffer, *output_hash);
}
std::pair<uint64_t, SyscallErrorCode>
syscall_GetMsgSender_outer(std::span<std::byte> contract_memory,
                           uint64_t user_output_sender, uint64_t /*unused*/,
                           uint64_t /*unused*/, uint64_t /*unused*/)
{
  ContractAddress *output_sender =
      check_user_argument<ContractAddress>(contract_memory, user_output_sender,
                                           sizeof(ContractAddress));

  if (!output_sender) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_GetMsgSender_inner(*output_sender);
}
std::pair<uint64_t, SyscallErrorCode>
syscall_BytestringGetLen_outer(std::span<std::byte> contract_memory,
                               uint64_t user_input_key,
                               uint64_t user_output_length, uint64_t /*unused*/,
                               uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  uint64_t *output_length =
      check_user_argument<uint64_t>(contract_memory, user_output_length,
                                    sizeof(uint64_t));

  if (!output_length) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_BytestringGetLen_inner(*input_key, *output_length);
}
std::pair<uint64_t, SyscallErrorCode>
syscall_BytestringGet_outer(std::span<std::byte> contract_memory,
                            uint64_t user_input_key,
                            uint64_t user_output_bytestring,
                            uint64_t user_output_bytestring_len,
                            uint64_t user_output_written_len)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  std::optional<std::span<std::byte>> output_bytestring =
      check_user_span_argument(contract_memory, user_output_bytestring,
                               user_output_bytestring_len);

  if (!output_bytestring) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  uint64_t *output_written_len =
      check_user_argument<uint64_t>(contract_memory, user_output_written_len,
                                    sizeof(uint64_t));

  if (!output_written_len) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_BytestringGet_inner(*input_key, *output_bytestring,
                                     *output_written_len);
}
std::pair<uint64_t, SyscallErrorCode>
syscall_BytestringSet_outer(std::span<std::byte> contract_memory,
                            uint64_t user_input_key,
                            uint64_t user_input_bytestring,
                            uint64_t user_input_bytestring_len,
                            uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  std::optional<std::span<const std::byte>> input_bytestring =
      check_user_span_argument(contract_memory, user_input_bytestring,
                               user_input_bytestring_len);

  if (!input_bytestring) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_BytestringSet_inner(*input_key, *input_bytestring);
}
std::pair<uint64_t, SyscallErrorCode>
syscall_HasKey_outer(std::span<std::byte> contract_memory,
                     uint64_t user_input_key, uint64_t user_output_status,
                     uint64_t /*unused*/, uint64_t /*unused*/)
{
  const StorageKey *input_key =
      check_user_argument<const StorageKey>(contract_memory, user_input_key,
                                            sizeof(StorageKey));

  if (!input_key) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  KeyStatus *output_status =
      check_user_argument<KeyStatus>(contract_memory, user_output_status,
                                     sizeof(KeyStatus));

  if (!output_status) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kFault};
  }

  return syscall_HasKey_inner(*input_key, *output_status);
}

using SyscallFunc = std::pair<uint64_t, SyscallErrorCode> (*)(
    std::span<std::byte>, uint64_t, uint64_t, uint64_t, uint64_t);

const std::array<std::pair<SyscallNo, SyscallFunc>, 6> syscall_table = {
    std::pair{SyscallNo::kSHA256, &syscall_SHA256_outer},
    std::pair{SyscallNo::kGetMsgSender, &syscall_GetMsgSender_outer},
    std::pair{SyscallNo::kBytestringGetLen, &syscall_BytestringGetLen_outer},
    std::pair{SyscallNo::kBytestringGet, &syscall_BytestringGet_outer},
    std::pair{SyscallNo::kBytestringSet, &syscall_BytestringSet_outer},
    std::pair{SyscallNo::kHasKey, &syscall_HasKey_outer},
};

// returns gas_consumed, syscall error>
std::pair<uint64_t, SyscallErrorCode>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  if (arg0 >= syscall_table.size()) {
    // invalid syscall
    return {kUnknownSyscallCost, SyscallErrorCode::kUnknownSyscall};
  }

  static_assert(syscall_table.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = syscall_table[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  return syscall_fn_outer(contract_memory, arg1, arg2, arg3, arg4);
}

HostFnStatus<uint64_t>
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  auto [gas_consumed, err_code] =
      syscall_entry_inner(call_context, arg0, arg1, arg2, arg3, arg4);

  if (!call_context->runtime->consume_gas(gas_consumed)) {
    return HostFnError{HostFnErrorCode::kOutOfGas};
  }
  return static_cast<uint64_t>(err_code);
}

} // namespace hogd
