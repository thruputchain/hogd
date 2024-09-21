#include <cstring>
#include <utility>

#include "hogd/syscall/syscall_entry.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/runtime/status.h"
#include "hogd/runtime/wasmi_runtime.h"
#include "hogd/syscall/gas_cost_table.h"
#include "syscall.hh"

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
T*
check_user_argument(std::span<std::byte> contract_memory, uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return nullptr;
  }
  return reinterpret_cast<T*>(contract_memory.data() + user_offset);
}

std::optional<std::span<std::byte>>
check_user_span_argument(std::span<std::byte> contract_memory, uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return std::nullopt;
  }
  return contract_memory.subspan(user_offset, user_len);
}

std::pair<uint64_t, SyscallErrorCode>
syscall_SHA256_inner(std::span<std::byte> hash_input, SHA256Hash& hash_out) {

  hash_out = sha256(hash_input);
  return {SHA256HashCost(hash_input.size()), SyscallErrorCode::kNone};
}

std::pair<uint64_t, SyscallErrorCode>
syscall_SHA256_outer(std::span<std::byte> contract_memory, 
  uint64_t user_input_buffer, uint64_t user_input_buffer_len,
  uint64_t user_output_hash, uint64_t)
{
  SHA256Hash* output_hash = check_user_argument<SHA256Hash>(
    contract_memory, user_output_hash, sizeof(SHA256Hash));

  std::optional<std::span<std::byte>> input_buffer 
    = check_user_span_argument(contract_memory, user_input_buffer, user_input_buffer_len);

  if (!output_hash) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kInvalidPtrError};
  }

  if (!input_buffer) {
    return {kGenericBadSyscallCost, SyscallErrorCode::kInvalidPtrError};
  }

  return syscall_SHA256_inner(*input_buffer, *output_hash);
}

// returns gas_consumed, syscall error>
std::pair<uint64_t, SyscallErrorCode>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  if (arg0 > INT32_MAX) {
    // invalid syscall -- xdr enums are int32-backed
    return {kUnknownSyscallCost, SyscallErrorCode::kUnknownSyscall};
  }

  auto syscall = static_cast<SyscallNo>(arg0);
  using enum SyscallNo;

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  switch (syscall) {
  case kSHA256:
    return syscall_SHA256_outer(contract_memory, arg1, arg2, arg3, arg4);
  }
  return {kUnknownSyscallCost, SyscallErrorCode::kUnknownSyscall};
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

}; // namespace hogd
