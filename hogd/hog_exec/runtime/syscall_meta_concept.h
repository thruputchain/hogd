#pragma once

#include <algorithm>
#include <concepts>
#include <coroutine>
#include <type_traits>

#include "syscall.hh"
#include "syscall_error.hh"

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
  // should deduce to an array of SyscallErrorCode
  syscall_meta.kAllowedErrors;

  // Can the syscall return to the guest environment
  { syscall_meta.kCanReturn } -> std::convertible_to<bool>;
};

// Used for test syscalls that do not have trace data infrastructure integrated
constexpr static SyscallNo kNoTracingSyscall = static_cast<SyscallNo>(-1);

namespace syscall_meta_detail {

// Return type of a function pointer. kInnerFunc is a constexpr static, so the
// pointer type is const.
template<typename Sig> struct func_return;
template<typename R, typename... Args> struct func_return<R (*)(Args...)> {
  using type = R;
};
template<typename R, typename... Args>
struct func_return<R (*)(Args...) noexcept> {
  using type = R;
};

template<typename Sig>
using func_return_t = typename func_return<std::remove_const_t<Sig>>::type;

// True when R is a legal coroutine return type. Async has a nested
// promise_type; MaybeAsync gets one from the std::coroutine_traits
// specialization in x23/async.h, which must be visible at the check.
template<typename R>
concept is_coroutine_return =
    requires { typename std::coroutine_traits<R>::promise_type; };

} // namespace syscall_meta_detail

// The declared return type of the syscall body.
template<is_syscall_meta SyscallMeta>
using SyscallBodyResult =
    syscall_meta_detail::func_return_t<decltype(SyscallMeta::kInnerFunc)>;

// The body returns its result directly. A coroutine return type is something
// the caller has to await.
template<is_syscall_meta SyscallMeta>
constexpr bool kSyscallBodyIsDirect =
    !syscall_meta_detail::is_coroutine_return<SyscallBodyResult<SyscallMeta>>;

class SyscallAllowedErrors {
public:
  SyscallAllowedErrors() = delete;

  // helper to check if a syscall error code is in the allowed list for a
  // syscall. Used in debug builds to validate that syscalls only return
  // documented errors.
  template<is_syscall_meta SyscallMeta>
  constexpr static bool is_allowed_error_code(SyscallErrorCode code)
  {
    // kSyscallErrorCodeNone is always allowed (success case)
    // kSyscallErrorCodeFault is allowed as all syscalls can be given an invalid
    // input
    if (code == SyscallErrorCode::kSyscallErrorCodeNone ||
        code == SyscallErrorCode::kSyscallErrorCodeFault) {
      return true;
    }

    // Mutable syscalls always allow kSyscallErrorCodeInvalidSyscallInput:
    // the read-only wrapper returns it when a mutable syscall is called in a
    // read-only frame, regardless of what the inner syscall documents.
    if constexpr (!SyscallMeta::kIsReadOnlySyscall) {
      if (code == SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
        return true;
      }
    }

    return std::ranges::any_of(SyscallMeta::kAllowedErrors,
                               [code](auto allowed) {
                                 return code == allowed;
                               });
  }
};

} // namespace hogd
