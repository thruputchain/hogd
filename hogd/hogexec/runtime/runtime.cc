#include "hogd/hogexec/runtime/runtime.h"

#include <memory>
#include <string_view>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasm/wasmi_runtime.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/wasm/wasmtime_runtime.h"
#include "x23/format.h"

namespace hogd {

// create RuntimeContext with no imported functions
std::unique_ptr<RuntimeContext>
make_runtime_context(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
  {
    // For now, wasmtime is used even when engine is kWasmi
    static WasmtimeGlobalContext wasmtime_global_context =
        WasmtimeGlobalContext(8);
    return std::make_unique<WasmtimeContext>(wasmtime_global_context);
  }
  default:
    return nullptr;
  }
}

namespace {
WasmtimeGlobalContext
make_wasmtime_hogexec_global_context()
{
  auto context = WasmtimeGlobalContext(8);
  auto ok = context.link_fn("marmot", "syscall", &syscall_entry);
  CHECK(ok);
  return context;
}

[[maybe_unused]]
WasmiGlobalContext
make_wasmi_hogexec_global_context()
{
  auto context = WasmiGlobalContext();
  auto ok = context.link_fn("marmot", "syscall", &syscall_entry);
  CHECK(ok);
  return context;
}

} // namespace

std::unique_ptr<RuntimeContext>
make_hogexec_wasm_context(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
  {
    // For now, wasmtime is used even when engine is kWasmi
    static WasmtimeGlobalContext wasmtime_global_context =
        make_wasmtime_hogexec_global_context();
    return std::make_unique<WasmtimeContext>(wasmtime_global_context);
  }
  default:
    return nullptr;
  }
}

InvokeResult
Runtime::invoke(std::string_view method_name, uint64_t gas_limit)
{
  // Push the previous gas limit onto the stack.
  const uint64_t prev_gas_limit = get_available_gas();
  set_available_gas(gas_limit);

  trace("Invoking method {} with gas limit {}", method_name, gas_limit);
  auto res = invoke_internal(method_name);
  uint64_t gas_remaining = get_available_gas();
  trace("Invoke result: {}, gas consumed: {}",
        res.has_value() ? "success" : x23::show(res.error().code()),
        gas_limit - gas_remaining);

  // Interpreters that perform metering on a basic block level, instead of
  // instruction by instruction, might not actually charge for the last block.
  // (The "consume fuel" operation is generally placed at the top of a basic
  // block. As far as I can tell, this is the case for wasmi). We hide this
  // detail by charging for the entire gas limit, if we reach an OutOfGas error.
  //
  // This makes the behavior of the wasm runtime consistent with the behavior of
  // consume_gas (defined in this file), which is what we use to consume gas
  // within syscalls.

  if (!res.has_value() &&
      res.error().code() == InvokeErrorCode::kInvokeOutOfGas) {
    gas_remaining = 0;
  }

  CHECK(gas_remaining <= gas_limit);
  uint64_t gas_consumed = gas_limit - gas_remaining;

  // Restore previous gas limit.
  set_available_gas(prev_gas_limit);

  return InvokeResult{.result_ = res, .gas_consumed_ = gas_consumed};
}

bool
Runtime::consume_gas(uint64_t gas) noexcept
{
  uint64_t current_gas = get_available_gas();

  if (current_gas >= gas) {
    set_available_gas(current_gas - gas);
    return true;
  }
  // If there is insufficient gas to consume the target amount,
  // we consume all remaining gas.
  set_available_gas(0);
  return false;
}

Runtime::Runtime(std::unique_ptr<HostCallContext> host_call_context)
  : host_call_context_(host_call_context.release())
{
  host_call_context_->runtime = this;
}

} // namespace hogd
