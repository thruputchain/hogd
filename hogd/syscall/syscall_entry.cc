#include "hogd/syscall/syscall_entry.h"

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/runtime/status.h"
#include "hogd/runtime/wasmi_runtime.h"
#include "hogd/syscall/gas_cost_table.h"

#include <cstring>
#include <utility>

#include "syscall.hh"

namespace hogd {

bool
check_memory_range(uint64_t memory_len, uint64_t base, uint64_t len)
{
  uint64_t base_end = 0;
  if (__builtin_add_overflow(base, len, &base_end)) {
    return false;
  }
  if (base_end >= memory_len) {
    return false;
  }
  return true;
}

std::optional<std::span<std::byte>>
get_memory_range(std::span<std::byte> memory, uint64_t offset, uint64_t len) {
  if (!check_memory_range(memory.size(), offset, len)) {
    return std::nullopt;
  }
  return memory.subspan(offset, len);
}

template<size_t length>
void
write_argument(std::span<std::byte> target, std::span<const std::byte, length> source)
{
  assert(target.size() >= source.size());
  std::memcpy(target.data(), source.data(), source.size());
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

  switch (syscall) {
  case kSHA256:
  {
    // arg1: memory offset
    // arg2: memory length
    // arg3: hash output offset

    auto memory = call_context->runtime->get_memory();

    auto input_range = get_memory_range(memory, arg1, arg2);
    auto output_range = get_memory_range(memory, arg3, 32);

    if (!input_range || !output_range) {
      return {kGenericBadSyscallCost, SyscallErrorCode::kInvalidPtrError};
    }

    SHA256Hash sha256_result = sha256(input_range);

    write_argument(output_range, std::as_bytes(std::span(sha256_result)));

    return {kSHA256Cost(input_range.size()), SyscallErrorCode::kNone};
  }
  default:
    return {kUnknownSyscallCost, SyscallErrorCode::kUnknownSyscall};
  }
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
