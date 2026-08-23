#include "hogd/hog_exec/runtime/runtime.h"

#include <exception>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "x23/async.h"
#include "x23/format.h"

namespace hogd {

x23::Async<InvokeResult>
Runtime::invoke(MethodID method_id, uint64_t gas_limit)
{

  // This runs on the child-Invoke path inside the noexcept syscall body. A
  // logger allocation/sink failure (typically std::bad_alloc out of memory)
  // must not throw across the noexcept boundary and terminate the process;
  // treat it as an unrecoverable invocation failure instead (issues #2713,
  // #2718).
  try {
    trace("Invoking method {} with gas limit {}", method_id, gas_limit);
  } catch (std::exception const &e) {
    report_dropped_log(e.what());
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = 0};
  } catch (...) {
    report_dropped_log("unknown exception");
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = 0};
  }
  // invoke_internal() is a coroutine in every implementation, so building it
  // allocates a frame in the ramp and can throw before the body runs. That is
  // the same class of failure the body itself reports structurally, so catch
  // it here rather than making implementations reestablish a noexcept boundary
  // each -- which a coroutine cannot do for its own frame.
  // Seeded only because the pair is not default-constructible.
  std::pair<InvokeStatus<void>, GasAmount> invoked{
      InvokeError{InvokeErrorCode::Unrecoverable}, 0};
  try {
    invoked = co_await invoke_internal(method_id, gas_limit);
  } catch (std::exception const &e) {
    error("Unrecoverable: could not run contract body: {}", e.what());
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = 0};
  } catch (...) {
    report_dropped_log("non-std exception running contract body");
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = 0};
  }
  auto [res, gas_consumed] = invoked;
  try {
    trace("Invoke result: {}, gas consumed: {}",
          res.has_value() ? "success" : x23::show(res.error().code()),
          gas_consumed);
  } catch (std::exception const &e) {
    report_dropped_log(e.what());
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = gas_consumed};
  } catch (...) {
    report_dropped_log("unknown exception");
    co_return InvokeResult{.result_ =
                               InvokeError{InvokeErrorCode::Unrecoverable},
                           .gas_consumed_ = gas_consumed};
  }

  // Interpreters that perform metering on a basic block level, instead of
  // instruction by instruction, might not actually charge for the last block.
  // (The "consume fuel" operation is generally placed at the top of a basic
  // block. As far as I can tell, this is the case for wasmi). We hide this
  // detail by charging for the entire gas limit, if we reach an OutOfGas error.
  //
  // This makes the behavior of the wasm runtime consistent with the behavior of
  // consume_gas (defined in this file), which is what we use to consume gas
  // within syscalls.
  if (!res.has_value() && res.error().code() == InvokeErrorCode::OutOfGas) {
    gas_consumed = gas_limit;
  }

  CHECK(gas_consumed <= gas_limit);

  co_return InvokeResult{.result_ = res, .gas_consumed_ = gas_consumed};
}

Runtime::Runtime(SystemCallHandler *syscall_handler)
  : syscall_handler_(syscall_handler)
{}

} // namespace hogd
