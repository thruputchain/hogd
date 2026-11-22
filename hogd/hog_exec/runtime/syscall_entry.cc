#include "hogd/hog_exec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_fallback.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "syscall.hh"
#include "syscall_error.hh"

namespace hogd {

namespace {

using SyscallFunc = MaybeAsyncSyscallResult (*)(SystemCallHandler &,
                                                ExecutionContext, SyscallNo,
                                                uint32_t, uint32_t, uint32_t,
                                                uint32_t, uint32_t, uint32_t,
                                                uint32_t) noexcept;

struct SyscallTableEntry {
  SyscallNo syscall_no;
  SyscallFunc func;
  // The syscall API minor that introduced the syscall (since_minor in
  // syscalls.yaml).
  uint32_t since_minor;
};

template<size_t... Is>
constexpr std::array<SyscallTableEntry, sizeof...(Is)>
make_syscall_array(std::index_sequence<Is...> /*unused*/)
{
  return {SyscallTableEntry{
      .syscall_no = static_cast<SyscallNo>(Is),
      .func = &wrap_outer<SyscallMeta<static_cast<SyscallNo>(Is)>>,
      .since_minor = SyscallMeta<static_cast<SyscallNo>(Is)>::kSinceMinor}...};
}

static_assert(
    std::same_as<decltype(&wrap_outer<NoSyscallFuncMeta>), SyscallFunc>,
    "wrap_outer<NoSyscallFuncMeta> is not a SyscallFunc");

constexpr auto kSyscallTable =
    make_syscall_array(std::make_index_sequence<kSyscallCount>{});

// returns gas_consumed, syscall error>
template<std::same_as<uint32_t>... Args>
MaybeAsyncSyscallResult
syscall_entry_inner(SystemCallHandler *syscall_handler,
                    ExecutionContext context, uint32_t syscall_no,
                    Args... args) noexcept
{
  SyscallFunc syscall_fn = nullptr;
  assert(syscall_handler);
  // A syscall introduced after the minor the guest was built against does not
  // exist for it: it gets exactly what a number this host does not implement
  // gets, so whether a call succeeds depends on the guest's declared API and
  // not on which build runs it.
  bool valid_syscall_no =
      syscall_no < kSyscallTable.size() &&
      kSyscallTable[syscall_no].since_minor <= context.syscall_api_minor;
  if (!valid_syscall_no) {
    // invalid syscall
    if (auto *stats = syscall_handler->get_transaction_stats()) {
      stats->invalid_syscall_count++;
    }
    syscall_fn = &wrap_outer<NoSyscallFuncMeta>;
  }
  else {
    const SyscallTableEntry &entry = kSyscallTable[syscall_no];
    assert(entry.syscall_no == static_cast<SyscallNo>(syscall_no));
    syscall_fn = entry.func;
  }
  assert(syscall_fn);

  static_assert(kSyscallTable.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  if (auto *stats = syscall_handler->get_transaction_stats()) {
    if (valid_syscall_no) {
      stats->syscall_counts[syscall_no]++;
    }
  }

  return syscall_fn(*syscall_handler, context,
                    static_cast<SyscallNo>(syscall_no), args...);
}

} // namespace

MaybeAsyncSyscallResult
syscall_entry(ExternHostCallContext call_context, ExecutionContext context,
              uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
              uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7)
{
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "type mismatch");

  return syscall_entry_inner(static_cast<SystemCallHandler *>(call_context),
                             context, arg0, arg1, arg2, arg3, arg4, arg5, arg6,
                             arg7);
}

} // namespace hogd
