#include "hogd/test_ffi/mock_runtime.h"

#include "hogd/base/coropoll.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

#include <cassert>
#include <memory>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"

namespace hogd {

void
MockRuntimeContext::set_trampoline(int32_t contract,
                                   ForeignEntryPoint entry_point,
                                   void *foreign_context)
{
  mocked_contracts_.insert_or_assign(contract, MockContract(entry_point,
                                                            foreign_context));
}

x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
MockRuntime::invoke_internal(MethodID method_id, uint64_t gas_limit)
{
  reset_gas(gas_limit);
  return invoke_off_pool(method_id);
}

x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
MockRuntime::invoke_off_pool(MethodID method_id)
{
  // ThreadPool::async() runs this on a thread outside the pool and resumes us
  // when it returns, so the blocking mock syscall entry underneath the guest
  // call is legal there. The handler context is installed on that thread
  // rather than this one for the same reason: the FFI call's thread_local slot
  // belongs to whichever thread entered the call, and neither this coroutine's
  // resumption thread nor the one below is necessarily it. Everything the mock
  // contract reaches -- mock_syscall_parser, the nested FFI entry points, a
  // nested invoke coming back through here -- reads it through get_context().
  auto res = co_await default_coropoll().pool_.async([this, method_id] {
    SyscallHandlerContext *context = owner_;
    SyscallHandlerContext::ScopedContext scoped_context(context);
    context->push_mock_runtime(this);
    // Pop on every exit path. MockContract::invoke() now catches the foreign
    // callback's exceptions and returns Unrecoverable instead of letting them
    // unwind (issue #2789), so the ordinary return path below does the popping.
    // Keep the RAII pop anyway: the callback type is not noexcept, and if any
    // throw ever does reach here the mock-runtime stack must still be restored
    // -- a leaked entry leaves the handler permanently uncloseable (#2470).
    x23::Defer pop_runtime([this, context]() {
      auto *runtime = context->pop_mock_runtime();
      assert(runtime == this);
      (void)runtime;
    });
    return backing_contract_.invoke(method_id);
  });
  co_return std::pair<InvokeStatus<void>, GasAmount>{res, gas_consumed()};
}

x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
MockRuntimeContext::make_new_runtime(DeployedContract const &deployed_contract,
                                     SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type == ContractType::kMockTest) {

    auto contract =
        co_await syscall_handler->resolve_contract(deployed_contract_hash.hash);
    if (!contract.has_value()) {
      co_return contract.error();
    }
    if (contract.value() == nullptr) {
      error("Unrecoverable: Contract lookup failed: {}",
            show_byte_array(deployed_contract_hash.hash));
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }

    if (contract.value()->contract_type() != ContractType::kMockTest) {
      error("Unrecoverable: Attempt made to access a non-mock test contract "
            "from MockRuntimeContext: {}",
            contract.value()->contract_type());
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }

    int32_t mock_contract = contract.value()->mock_contract_id();
    auto it = mocked_contracts_.find(mock_contract);
    if (it != mocked_contracts_.end()) {
      // The handler is the one hog_exec is running, so the context around it
      // is the harness state this runtime belongs to. No dependence on which
      // thread resumed this coroutine, and correct when several handlers share
      // one runtime context.
      co_return std::make_unique<MockRuntime>(
          it->second, SyscallHandlerContext::from_handler(syscall_handler));
    }
    co_return nullptr;
  }
  // otherwise some other contract type; returns whatever the base context
  // wants to do with the contract.
  co_return co_await base_context_->make_new_runtime(deployed_contract,
                                                     syscall_handler);
}

} // namespace hogd
