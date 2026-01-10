#pragma once

#include <concepts>

#include "syscall.hh"

namespace hogd {

template<typename SyscallMeta>
concept is_syscall_meta = requires(SyscallMeta syscall_meta) {
  { syscall_meta.kSyscallNo } -> std::convertible_to<SyscallNo>;
  { syscall_meta.kIsReadOnlySyscall } -> std::convertible_to<bool>;
  // should deduce to a function pointer
  syscall_meta.kInnerFunc;
  // should deduce to an array of string_view
  syscall_meta.kInputNames;
  // should deduce to an array of string_view
  syscall_meta.kOutputNames;

  // Can the syscall return to the wasm environment
  { syscall_meta.kCanReturn } -> std::convertible_to<bool>;
};

} // namespace hogd
