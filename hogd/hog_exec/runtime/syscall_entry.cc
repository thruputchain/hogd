#include "hogd/hog_exec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_fallback.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "hogd/hog_exec/runtime/trampoline_result.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

namespace {

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> (*)(
    SystemCallHandler &, WasmContext, SyscallNo, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t) noexcept;

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
template<std::same_as<uint32_t>... Args>
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_entry_inner(SystemCallHandler *syscall_handler,
                    WasmContext wasm_context, uint32_t syscall_no,
                    Args... args) noexcept
{
  SyscallFunc syscall_fn = nullptr;
  assert(syscall_handler);
  bool valid_syscall_no = syscall_no < kSyscallTable.size();
  if (!valid_syscall_no) {
    // invalid syscall
    if (auto *stats = syscall_handler->get_transaction_stats()) {
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

  if (auto *stats = syscall_handler->get_transaction_stats()) {
    if (valid_syscall_no) {
      stats->syscall_counts[syscall_no]++;
    }
  }

  return syscall_fn(*syscall_handler, wasm_context,
                    static_cast<SyscallNo>(syscall_no), args...);
}

} // namespace

extern "C" TrampolineResult
syscall_entry(ExternHostCallContext call_context, WasmContext wasm_context,
              uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
              uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7)
{
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "type mismatch");

  auto [err, gas_consumed] =
      syscall_entry_inner(static_cast<SystemCallHandler *>(call_context),
                          wasm_context, arg0, arg1, arg2, arg3, arg4, arg5,
                          arg6, arg7);
  return to_trampoline_result(err, gas_consumed);
}

} // namespace hogd
