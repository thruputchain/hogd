#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "syscall.hh"

namespace hogd {

namespace {

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> (*)(
    SystemCallHandler &, Runtime &, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

template<size_t... Is>
constexpr std::array<std::pair<SyscallNo, SyscallFunc>, sizeof...(Is)>
make_syscall_array(std::index_sequence<Is...>)
{
  return {std::pair<SyscallNo, SyscallFunc>{
      static_cast<SyscallNo>(Is),
      &wrap_outer<static_cast<SyscallNo>(Is),
                  SyscallMeta<static_cast<SyscallNo>(Is)>::kInnerFunc>}...};
}

constexpr auto kSyscallTable =
    make_syscall_array(std::make_index_sequence<kSyscallCount>{});

// returns gas_consumed, syscall error>
template<std::same_as<uint64_t>... Args>
std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t syscall_no,
                    Args... args)
{
  if (syscall_no >= kSyscallTable.size()) {
    // invalid syscall
    if (auto *stats = call_context->syscall_handler->get_transaction_stats()) {
      stats->invalid_syscall_count++;
    }
    return {SyscallErrorCode::kNoSyscall, kNoSyscallCost};
  }

  static_assert(kSyscallTable.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = kSyscallTable[syscall_no];

  assert(syscall_expect == static_cast<SyscallNo>(syscall_no));
  assert(call_context->syscall_handler);

  if (auto *stats = call_context->syscall_handler->get_transaction_stats()) {
    stats->syscall_counts[syscall_no]++;
  }

  return syscall_fn_outer(*call_context->syscall_handler,
                          *call_context->runtime, args...);
}

} // namespace

HostFnStatus<SyscallErrorCode>
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
              uint64_t arg6, uint64_t arg7)
{
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "type mismatch");

  auto [err, gas_consumed] = syscall_entry_inner(call_context, arg0, arg1, arg2,
                                                 arg3, arg4, arg5, arg6, arg7);
  if (!call_context->runtime->consume_gas(gas_consumed)) {
    return {HostFnError{HostFnErrorCode::kHostOutOfGas}};
  }
  return err;
}

} // namespace hogd
