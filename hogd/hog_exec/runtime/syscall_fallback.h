#pragma once

#include <array>
#include <string_view>
#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_meta_concept.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"

namespace hogd {

// Reject disabled syscalls before parsing or tracing their operands.
constexpr bool
syscall_is_enabled(uint64_t syscall)
{
#ifndef HOGD_EXPERIMENTAL_RISTRETTO_MSM
  return syscall != static_cast<uint64_t>(SyscallNo::kRistretto255Msm);
#else
  (void)syscall;
  return true;
#endif
}

constexpr std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
no_syscall_func_inner()
{
  return {SyscallErrorCode::kSyscallErrorCodeNoSyscall,
          WrappedGasAmount{kNoSyscallCost}};
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
