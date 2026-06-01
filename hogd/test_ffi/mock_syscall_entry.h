#pragma once

#include <array>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_fallback.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"

namespace hogd {

using MockSyscallFunc = ForeignHostFnResult (*)(SyscallNo, uint64_t, uint64_t,
                                                uint64_t, uint64_t, uint64_t,
                                                uint64_t, uint64_t);

template<size_t... Is>
constexpr std::array<MockSyscallFunc, sizeof...(Is)>
make_mock_syscall_array(std::index_sequence<Is...> /*unused*/)
{
  return {&mock_syscall_parser<SyscallMeta<static_cast<SyscallNo>(Is)>>...};
}

constexpr auto kMockSyscallTable =
    make_mock_syscall_array(std::make_index_sequence<kSyscallCount>{});

inline ForeignHostFnResult
mock_syscall_entrypoint(uint64_t syscall, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5,
                        uint64_t arg6, uint64_t arg7) noexcept
{
  MockSyscallFunc mock_syscall_func = &mock_syscall_parser<NoSyscallFuncMeta>;
  if (syscall < kMockSyscallTable.size()) {
    mock_syscall_func = kMockSyscallTable[syscall];
  }
  // The parser path can allocate (e.g. appending a record when syscall tracing
  // is enabled). An honest C API caller must never see an allocation failure
  // (or any other exception) escape this noexcept boundary and abort the
  // process; translate it into a structured unrecoverable FFI error instead.
  //
  // Deliberately no logging in the catch path: the exception we most expect
  // here is std::bad_alloc, and the logger formats into a std::string, so it
  // can allocate and throw again under the very memory pressure that triggered
  // this path. A second throw would re-cross this noexcept boundary and
  // terminate the caller anyway, so the handler must stay allocation-free.
  try {
    return mock_syscall_func(static_cast<SyscallNo>(syscall), arg1, arg2, arg3,
                             arg4, arg5, arg6, arg7);
  } catch (...) {
    return unrecoverable_error();
  }
}

} // namespace hogd
