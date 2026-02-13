#pragma once

#include <array>
#include <string_view>
#include <utility>

#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

constexpr std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
no_syscall_func_inner() noexcept
{
  return {SyscallErrorCode::kSyscallErrorCodeNoSyscall, kNoSyscallCost};
}

struct NoSyscallFuncMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static auto kInnerFunc = &no_syscall_func_inner;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 1> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeNoSyscall};
};

static_assert(is_syscall_meta<NoSyscallFuncMeta>);

} // namespace hogd
