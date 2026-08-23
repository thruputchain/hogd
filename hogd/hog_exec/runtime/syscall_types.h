#pragma once

#include <cstdio>
#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class WrappedGasAmount {
public:
  constexpr explicit WrappedGasAmount(GasAmount gas_amount)
    : gas_amount_(gas_amount)
  {}
  constexpr GasAmount get() const { return gas_amount_; }

private:
  GasAmount gas_amount_;
};

// What an inner syscall body produces: the host-function status the sandbox
// observes, plus the gas the body wants charged.
using SyscallInnerResult =
    std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>;

// What wrap_after_parse() awaits.  Every inner body is a coroutine, so every
// one of them allocates a frame -- including the bodies that could answer
// without waiting.  x23::MaybeAsync exists to skip that allocation, but its
// value arm cannot be mixed with the Async arm here: the awaiting frame is
// destroyed from inside the resumption chain the completing body hands it,
// which leaves the variant destroying a coroutine handle that is no longer a
// frame.  A frame per syscall is the price of that being correct.
using AsyncSyscallInnerResult = x23::Async<SyscallInnerResult>;

// Report a syscall-trace diagnostic straight to stderr without allocating (same
// fallback pattern as test_ffi's do_ffi_trace).
//
// Syscall bodies run inside a deliberately noexcept lambda in
// wrap_after_parse() (noexcept so gas accounting and return-state validation
// can never be skipped). Several trace steps inside that boundary allocate and
// can throw std::bad_alloc; an escaping exception would terminate the process
// before the caller's structured-error boundary (mock_syscall_entrypoint() /
// wrap_outer()) could translate it. Because logging the failure also allocates
// (and could throw again), the diagnostic goes through this no-allocation
// reporter, which only runs on the (cold) failure path. Two kinds of caller use
// it:
//   - wrap_after_parse()'s trace-input copy and trace-log formatting (#2561,
//     #2562) are best-effort diagnostics: they report and drop the record/log
//     line, then let the syscall finish with its real result (mirroring
//     SystemCallHandler::start_frame()'s drop-and-continue, #2473).
//   - wrap_after_parse()'s exception boundary around the inner syscall bodies
//     (issue #3400): the bodies themselves are not noexcept and may throw
//     freely (including from the XDR trace-output emplace); the boundary
//     translates the exception to HostFnErrorCode::Unrecoverable and falls
//     back to this reporter if formatting its own diagnostic throws.
inline void
report_dropped_syscall_trace(char const *what) noexcept
{
  std::fputs("error tracing syscall: ", stderr);
  std::fputs(what, stderr);
  std::fputc('\n', stderr);
}

} // namespace hogd
