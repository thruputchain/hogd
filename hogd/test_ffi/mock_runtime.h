#pragma once

#include <limits>
#include <map>
#include <memory>
#include <utility>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"

namespace hogd {

class MockRuntime;

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

  SystemStatus<std::unique_ptr<Runtime>>
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

  std::unique_ptr<MockRuntimeContext> clone() const
  {
    return std::make_unique<MockRuntimeContext>(base_context_,
                                                mocked_contracts_);
  }

private:
  std::map<int32_t, MockContract> mocked_contracts_;
  std::shared_ptr<RuntimeContext> base_context_;
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
    return false;
  }

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
};

class MockRuntime : public MockGasRuntime {
public:
  MockRuntime(MockContract contract) : backing_contract_(std::move(contract)) {}

  ~MockRuntime() override = default;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;

  MockContract backing_contract_;
};

class EmptyRuntime : public MockGasRuntime {
public:
  EmptyRuntime(Invocation const &invocation)
    : MockGasRuntime(invocation.gas_limit)
  {}

  ~EmptyRuntime() override = default;

protected:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID /*unused*/, uint64_t /*gas_limit*/) override
  {
    panic("unreachable: EmptyRuntime::invoke_internal called");
  }
};

} // namespace hogd
