#pragma once

#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/test_bindings.h"

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

  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;

  std::unique_ptr<Runtime> make_new_runtime(Contract const &contract,
                                            SystemCallHandler *syscall_handler,
                                            bool do_contract_verify) override;

  bool verify_contract(Contract const & /*unused*/) const override
  {
    return true;
  };

  // Sets the function pointer and context that a mocked contract calls into.
  void set_trampoline(uint32_t contract, ForeignEntryPoint entry_point,
                      void *foreign_context);

private:
  std::map<uint32_t, MockContract> mocked_contracts_;
  std::shared_ptr<RuntimeContext> base_context_;
};

// One instance of a mock contract.
// All invocations are forwarded to the MockContract instance,
// which is essentially a wrapper around a function pointer.
// Note that mocked contracts do not on their own track gas usage,
// aside from that explicitly consumed by syscalls.
//
// Also note that the host_call_context_ in a mock contract
// does NOT keept track of a SystemCallHandler* pointer.
// The mock testing system uses a separate method for tracking SystemCallHandler* context.
class MockRuntime : public Runtime {
public:
  MockRuntime(MockContract contract)
    : Runtime(std::make_unique<HostCallContext>()),
      backing_contract_(std::move(contract))
  {}

  ~MockRuntime() override = default;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override
  {
    return gas_remaining_;
  }

  void set_available_gas(uint64_t gas) noexcept override
  {
    gas_remaining_ = gas;
  }

  std::span<std::byte> get_memory() noexcept override { std::terminate(); }

private:
  InvokeStatus<int32_t> invoke_internal(std::string_view method_name) override;

  MockContract backing_contract_;
  uint64_t gas_remaining_ = 0;
};

} // namespace hogd
