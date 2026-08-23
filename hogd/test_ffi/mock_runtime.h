#pragma once

#include <limits>
#include <map>
#include <memory>
#include <utility>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

class MockRuntime;
class SyscallHandlerContext;

// Context for a runtime that has been mocked out to some function pointer.
// Returns a MockRuntime when the input to make_new_runtime is an appropriate
// mock contract. Otherwise, defaults to the kind of runtime returned by
// base_context.
class MockRuntimeContext : public RuntimeContext {
public:
  MockRuntimeContext(std::shared_ptr<RuntimeContext> base_context)
    : base_context_(std::move(base_context))
  {}

  MockRuntimeContext(std::shared_ptr<RuntimeContext> base_context,
                     std::map<int32_t, MockContract> mocked_contracts)
    : mocked_contracts_(std::move(mocked_contracts)),
      base_context_(std::move(base_context))
  {}

  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override
  {
    return base_context_->verify_contract(contract);
  };

  // Sets the function pointer and context that a mocked contract calls into.
  void set_trampoline(int32_t contract, ForeignEntryPoint entry_point,
                      void *foreign_context);

  bool has_mock_contract(int32_t id) const
  {
    return mocked_contracts_.contains(id);
  }

  // The handler context currently executing against this runtime context, or
  // nullptr between transactions. Set by SyscallHandlerContext::push_context()
  // on the thread entering the FFI call, and read back by make_new_runtime()
  // from inside the coroutine, where a thread_local would be empty.
  void set_handler_context(SyscallHandlerContext *ctx)
  {
    // Exactly one handler runs against a runtime context at a time: a clear
    // must follow a set, and a set must not overwrite a live one.
    CHECK((ctx == nullptr) != (handler_context_ == nullptr));
    handler_context_ = ctx;
  }

  std::unique_ptr<MockRuntimeContext> clone() const
  {
    return std::make_unique<MockRuntimeContext>(base_context_,
                                                mocked_contracts_);
  }

private:
  std::map<int32_t, MockContract> mocked_contracts_;
  std::shared_ptr<RuntimeContext> base_context_;
  SyscallHandlerContext *handler_context_ = nullptr;
};

// One instance of a mock contract.
// All invocations are forwarded to the MockContract instance,
// which is essentially a wrapper around a function pointer.
// Note that mocked contracts do not on their own track gas usage,
// aside from that explicitly consumed by syscalls.
//
// Note that a mock contract does NOT keep track of a SystemCallHandler*
// pointer (it is constructed with a null handler). The mock testing system
// uses a separate method for tracking SystemCallHandler* context.
class MockGasRuntime : public Runtime {
public:
  GasAmount gas_remaining() const { return gas_remaining_; }

  GasAmount gas_consumed() const
  {
    assert(gas_remaining_ <= initial_gas_);
    return initial_gas_ - gas_remaining_;
  }

  bool consume_gas(GasAmount gas)
  {
    if (gas < gas_remaining_) {
      gas_remaining_ -= gas;
      return true;
    }
    gas_remaining_ = 0;
    out_of_gas_ = true;
    return false;
  }

  // True once any debit against this frame has failed. Both paths that can
  // exhaust a frame set it:
  //
  //   * a syscall that outspends the frame's own budget (wrap_after_parse()'s
  //     gas epilogue), and
  //   * the charge for a child frame the frame could not pay for (the parent
  //     debit in finish_mock_invocation_frame).
  //
  // Production has no "keep running after out of gas" state: the OutOfGas host
  // error traps the frame's guest execution, so the frame always faults,
  // whichever path exhausted it. The mock pathway has no guest execution to
  // trap and instead lets its caller declare each frame's outcome, so the frame
  // records the fact here and its finish overrides that declaration (#2725).
  //
  // The syscall path does also return panic == OutOfGas to the harness caller,
  // but that only helps a caller that acts on it: a caller that ignored it
  // still finished the frame Return and committed ExecResultOK.
  bool is_out_of_gas() const { return out_of_gas_; }

protected:
  explicit MockGasRuntime(
      GasAmount initial_gas = std::numeric_limits<GasAmount>::max())
    : Runtime(nullptr), initial_gas_(initial_gas), gas_remaining_(initial_gas)
  {}

  void reset_gas(GasAmount initial_gas)
  {
    initial_gas_ = initial_gas;
    gas_remaining_ = initial_gas;
  }

private:
  GasAmount initial_gas_;
  GasAmount gas_remaining_;
  bool out_of_gas_{false};
};

class MockRuntime : public MockGasRuntime {
public:
  // [owner] is the handler context this runtime executes under. It is captured
  // here rather than read from the thread_local slot inside invoke_internal,
  // which runs on whichever thread the coroutine resumed on.
  MockRuntime(MockContract contract, SyscallHandlerContext *owner)
    : backing_contract_(std::move(contract)), owner_(owner)
  {
    CHECK(owner_ != nullptr);
  }

  ~MockRuntime() override = default;

private:
  x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;

  // Body of invoke_internal(). The guest call it runs is synchronous down to
  // the mock syscall entry, which blocks on an Async -- illegal on a thread
  // pool worker, and this coroutine may well be resumed onto one. So the call
  // goes to a thread outside the pool and this awaits it.
  x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_off_pool(MethodID method_id);

  MockContract backing_contract_;
  SyscallHandlerContext *owner_;
};

class EmptyRuntime : public MockGasRuntime {
public:
  EmptyRuntime(Invocation const &invocation)
    : MockGasRuntime(invocation.gas_limit)
  {}

  ~EmptyRuntime() override = default;

protected:
  x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_internal(MethodID /*unused*/, uint64_t /*gas_limit*/) override
  {
    panic("unreachable: EmptyRuntime::invoke_internal called");
  }
};

} // namespace hogd
