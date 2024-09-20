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
  if (__builtin_add_overflow_p(base, len, static_cast<uint64_t>(0))) {
    return false;
  }
  if (base + len >= memory_len) {
    return false;
  }
  return true;
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

    if (!check_memory_range(memory.size(), arg1, arg2)) {
      return {kGenericBadSyscallCost, SyscallErrorCode::kInvalidPtrError};
    }

    auto input_range = memory.subspan(arg1, arg2);

    if (!check_memory_range(memory.size(), arg3, 32)) {
      return {kGenericBadSyscallCost, SyscallErrorCode::kInvalidPtrError};
    }

    auto output_range = memory.subspan(arg3, 32);

    SHA256Hash sha256_result = sha256(input_range);

    static_assert(sha256_result.size() == 32);

    std::memcpy(output_range.data(), sha256_result.data(),
                sha256_result.size());

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
