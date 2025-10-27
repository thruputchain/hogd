#pragma once

#include <array>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"

namespace hogd {

template<typename SyscallMeta>
ForeignHostFnResult
mock_syscall_wrapper(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                     uint64_t arg5, uint64_t arg6, uint64_t arg7) noexcept
{
  return mock_syscall_parser<SyscallMeta>(arg1, arg2, arg3, arg4, arg5, arg6,
                                          arg7);
}

using MockSyscallFunc = ForeignHostFnResult (*)(uint64_t, uint64_t, uint64_t,
                                                uint64_t, uint64_t, uint64_t,
                                                uint64_t);

template<size_t... Is>
constexpr std::array<MockSyscallFunc, sizeof...(Is)>
make_mock_syscall_array(std::index_sequence<Is...> /*unused*/)
{
  return {&mock_syscall_wrapper<SyscallMeta<static_cast<SyscallNo>(Is)>>...};
}

constexpr auto kMockSyscallTable =
    make_mock_syscall_array(std::make_index_sequence<kSyscallCount>{});

inline ForeignHostFnResult
mock_syscall_entrypoint(uint64_t syscall, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5,
                        uint64_t arg6, uint64_t arg7) noexcept
{
  return kMockSyscallTable[syscall](arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

} // namespace hogd
