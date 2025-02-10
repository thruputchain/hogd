#pragma once

#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/mock_contract.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/test_bindings.h"
#include "types.hh"

namespace hogd {

class MockRuntime;

class MockRuntimeContext : public RuntimeContext {
public:
  MockRuntimeContext(std::shared_ptr<RuntimeContext> base_context)
    : base_context_(std::move(base_context))
  {}

  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;

  std::unique_ptr<Runtime>
  make_new_runtime(SHA256Hash const &hash, const Contract *contract,
                   SystemCallHandler *syscall_handler) override;

  void set_trampoline(SHA256Hash const &contract, ForeignEntryPoint entry_point,
                      void *foreign_context);

private:
  std::map<SHA256Hash, MockContract> mocked_contracts_;
  std::shared_ptr<RuntimeContext> base_context_;
};

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
  std::span<const std::byte> get_memory() const noexcept override
  {
    std::terminate();
  }

private:
  InvokeStatus<int32_t>
  invoke_internal(std::string const &method_name) override;

  MockContract backing_contract_;
  uint64_t gas_remaining_ = 0;
};

} // namespace hogd
