#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_fallback.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "syscall.hh"
#include "syscall_error.hh"

namespace hogd {

namespace {

using SyscallFunc = HostFnStatus<SyscallErrorCode> (*)(
    SystemCallHandler &, Runtime &, LinearMemorySlice, SyscallNo, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t) noexcept;

template<size_t... Is>
constexpr std::array<std::pair<SyscallNo, SyscallFunc>, sizeof...(Is)>
make_syscall_array(std::index_sequence<Is...> /*unused*/)
{
  return {std::pair<SyscallNo, SyscallFunc>{
      static_cast<SyscallNo>(Is),
      &wrap_outer<SyscallMeta<static_cast<SyscallNo>(Is)>>}...};
}

static_assert(
    std::same_as<decltype(&wrap_outer<NoSyscallFuncMeta>), SyscallFunc>,
    "wrap_outer<NoSyscallFuncMeta> is not a SyscallFunc");

constexpr auto kSyscallTable =
    make_syscall_array(std::make_index_sequence<kSyscallCount>{});

// returns gas_consumed, syscall error>
template<std::same_as<uint64_t>... Args>
HostFnStatus<SyscallErrorCode>
syscall_entry_inner(HostCallContext *call_context, LinearMemorySlice contract_memory, uint64_t syscall_no,
                    Args... args) noexcept
{
  SyscallFunc syscall_fn = nullptr;
  assert(call_context->syscall_handler);
  bool valid_syscall_no = syscall_no < kSyscallTable.size();
  if (!valid_syscall_no) {
    // invalid syscall
    if (auto *stats = call_context->syscall_handler->get_transaction_stats()) {
      stats->invalid_syscall_count++;
    }
    syscall_fn = &wrap_outer<NoSyscallFuncMeta>;
  }
  else {
    auto [syscall_expect, syscall_fn_outer] = kSyscallTable[syscall_no];
    assert(syscall_expect == static_cast<SyscallNo>(syscall_no));
    syscall_fn = syscall_fn_outer;
  }
  assert(syscall_fn);

  static_assert(kSyscallTable.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  if (auto *stats = call_context->syscall_handler->get_transaction_stats()) {
    if (valid_syscall_no) {
      stats->syscall_counts[syscall_no]++;
    }
  }

  return syscall_fn(*call_context->syscall_handler, *call_context->runtime, contract_memory,
                    static_cast<SyscallNo>(syscall_no), args...);
}

} // namespace

extern "C" TrampolineResult
syscall_entry(ExternHostCallContext call_context, LinearMemorySlice contract_memory, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
              uint64_t arg6, uint64_t arg7, uint64_t arg8, uint64_t arg9,
              uint64_t arg10)
{
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "type mismatch");

  auto err = syscall_entry_inner(reinterpret_cast<HostCallContext *>(call_context), contract_memory,arg0, arg1, arg2, arg3, arg4,
                                 arg5, arg6, arg7, arg8, arg9, arg10);
  return to_trampoline_result(err);
}

} // namespace hogd
