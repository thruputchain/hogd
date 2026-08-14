#include "hogd/test_ffi/mock_contract.h"

#include <exception>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

namespace {

// Best-effort diagnostic for an exception escaping a mock contract callback.
// The logger formats into a std::string and so can itself allocate; under the
// std::bad_alloc this path may handle, it can throw again. The caller has
// already caught an exception on a path that must not throw any further, so
// guard the log call: emit the message when memory allows, and on a nested
// logging failure fall back to the non-allocating stderr breadcrumb. Mirrors
// log_mock_syscall_escape() in mock_syscall_entry.h.
void
log_callback_escape(Runtime::MethodID method_id, char const *what) noexcept
{
  try {
    if (what != nullptr) {
      error("Unrecoverable: exception escaped mock contract callback for "
            "method {}: {}",
            method_id, what);
    }
    else {
      error("Unrecoverable: unknown exception escaped mock contract callback "
            "for method {}",
            method_id);
    }
  } catch (...) {
    report_dropped_log("exception escaped mock contract callback");
  }
}

} // namespace

InvokeStatus<void>
MockContract::invoke(Runtime::MethodID method_id)
{
  // ForeignEntryPoint is declared in a C header, but that only fixes its
  // linkage and ABI -- it does not stop a callback from throwing, and the
  // typedef is not noexcept. A callback compiled as C++ therefore can throw,
  // which is exactly how the harness is driven in-process: this test suite's
  // own callbacks, and the C++ PoC drivers on the issues below. (A C callback
  // has no exceptions to throw, and the Rust harness in sdk_rs registers an
  // `extern "C"` trampoline, so a Rust panic aborts at Rust's own unwind shim
  // and never reaches us. Neither can trip this path.)
  //
  // This is the only place the callback is called, and every path that reaches
  // it runs below a noexcept C++ boundary that a throw would terminate the
  // process at:
  //
  //   * root_invoke() -- the root mock frame's own callback (#2693),
  //   * a nested Invoke syscall from a Wasmtime or RV32 parent, which runs in
  //     syscall_Invoke_inner(...) noexcept below the Wasmtime host import /
  //     RV32 ecall trampoline, and
  //   * mock_syscall_entrypoint(kInvoke, ...), whose own catch sits above
  //     wrap_after_parse()'s noexcept syscall body.
  //
  // Only the first of those had a catch above it, so the other paths crashed
  // an honest harness caller (#2789). Catching here covers all of them at once
  // and keeps the outer catches as backstops: translate any throw into the
  // structured Unrecoverable result the callback could have returned itself,
  // and let the frame fail through the normal error path -- finish_frame() and
  // MockRuntime::invoke_internal()'s mock-runtime stack pop still run, rather
  // than being skipped by a stack unwind.
  ForeignInvokeResult res{};
  try {
    res = entry_point_(foreign_context_, method_id);
  } catch (std::exception const &e) {
    log_callback_escape(method_id, e.what());
    return InvokeError{InvokeErrorCode::Unrecoverable};
  } catch (...) {
    // Callbacks are not restricted to std::exception throws either (#2693).
    log_callback_escape(method_id, nullptr);
    return InvokeError{InvokeErrorCode::Unrecoverable};
  }

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::None) {
    return {};
  }
  return InvokeError{code};
}
} // namespace hogd
