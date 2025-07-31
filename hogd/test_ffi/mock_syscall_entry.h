#pragma once

#include <array>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "syscall_args.hh"

namespace hogd {

template<SyscallNo Syscall>
ForeignHostFnResult
mock_syscall_wrapper(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                     uint64_t arg5, uint64_t arg6, uint64_t arg7) noexcept
{
  constexpr static auto kInnerSyscall = SyscallMeta<Syscall>::kInnerFunc;

  constexpr static size_t kSyscallIdx = std::to_underlying(Syscall) + 1;

  using InputType = std::remove_cvref_t<decltype(get<kSyscallIdx>(
      std::declval<SyscallInput>()))>;
  using OutputType = std::remove_cvref_t<decltype(get<kSyscallIdx>(
      std::declval<SyscallOutput>()))>;

  return mock_syscall_parser<Syscall, InputType, OutputType, kInnerSyscall>(
      arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

using MockSyscallFunc = ForeignHostFnResult (*)(uint64_t, uint64_t, uint64_t,
                                                uint64_t, uint64_t, uint64_t,
                                                uint64_t);

template<size_t... Is>
constexpr std::array<MockSyscallFunc, sizeof...(Is)>
make_mock_syscall_array(std::index_sequence<Is...> /*unused*/)
{
  return {&mock_syscall_wrapper<static_cast<SyscallNo>(Is)>...};
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
