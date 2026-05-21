#pragma once

#include <map>
#include <memory>
#include <string_view>
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
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

  bool verify_contract(Contract const &contract) const override
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
// Also note that the host_call_context_ in a mock contract
// does NOT keep track of a SystemCallHandler* pointer.
// The mock testing system uses a separate method for tracking
// SystemCallHandler* context.
class MockRuntime : public Runtime {
public:
  MockRuntime(MockContract contract)
    : Runtime(std::make_unique<HostCallContext>()),
      backing_contract_(std::move(contract))
  {}

  ~MockRuntime() override = default;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(std::string_view method_name, uint64_t gas_limit) override;

  MockContract backing_contract_;
};

class EmptyRuntime : public Runtime {
public:
  EmptyRuntime(Invocation const & /*invocation*/)
    : Runtime(std::make_unique<HostCallContext>())
  {}

  ~EmptyRuntime() override = default;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(std::string_view /*unused*/,
                  uint64_t /*gas_limit*/) override
  {
    panic("unreachable: EmptyRuntime::invoke_internal called");
  }
};

} // namespace hogd
