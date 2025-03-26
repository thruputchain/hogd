#include "hogd/hogexec/runtime/syscall_entry.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall.hh"

namespace hogd {

int syscall_logfd = -1;

namespace {
// returns gas_consumed, syscall error>
std::pair<HostFnResult, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
                    uint64_t arg6)
{
  if (arg0 >= kSyscallTable.size()) {
    // invalid syscall
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kNoSyscall)},
            kNoSyscallCost};
  }

  static_assert(kSyscallTable.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = kSyscallTable[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));
  assert(call_context->syscall_handler);

  return syscall_fn_outer(*call_context->syscall_handler, *call_context->runtime, arg1,
                          arg2, arg3, arg4, arg5, arg6);
}

} // namespace

HostFnResult
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
              uint64_t arg6)
{
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "type mismatch");

  auto [err, gas_consumed] = syscall_entry_inner(call_context, arg0, arg1, arg2,
                                                 arg3, arg4, arg5, arg6);
  if (!call_context->runtime->consume_gas(gas_consumed)) {
    return {HostFnErrorCode::kOutOfGas, 0};
  }
  return err;
}

} // namespace hogd
