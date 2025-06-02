#include "hogd/hogexec/runtime/runtime.h"

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

std::shared_ptr<RuntimeContext>
make_runtime_context(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
    return std::make_shared<WasmiContext>();
    break;
  default:
    return nullptr;
  }
}

std::shared_ptr<RuntimeContext>
make_hogexec_wasm_context(RuntimeEngine engine)
{
  std::shared_ptr<RuntimeContext> out = make_runtime_context(engine);
  if (out) {
    out->link_fn("marmot", "syscall", &syscall_entry);
  }
  return out;
}

std::shared_ptr<RuntimeContext>
make_hogexec_context()
{
  return std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kWasmi,
           make_hogexec_wasm_context(RuntimeEngine::kWasmi)},
          // TODO(geoff): add hogexec native contracts here
      });
}

std::string
engine_to_string(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
    return "Wasmi";
  default:
    return "UnknownEngine";
  }
}

InvokeResult
Runtime::invoke(std::string_view method_name, uint64_t gas_limit)
{
  uint64_t prev_gas_limit = get_available_gas();
  set_available_gas(gas_limit);

  auto res = invoke_internal(method_name);

  uint64_t gas_remaining = get_available_gas();
  assert(gas_remaining <= gas_limit);

  uint64_t gas_consumed = gas_limit - gas_remaining;

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
  set_available_gas(0);
  return false;
}

Runtime::Runtime(std::unique_ptr<HostCallContext> host_call_context)
  : host_call_context_(host_call_context.release())
{
  host_call_context_->runtime = this;
}

} // namespace hogd
