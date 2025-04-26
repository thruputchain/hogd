#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

class SystemCallHandler;

// A single native contract.
// Native contracts run within the trust boundary of the system call handler;
// as such, they cannot be generated from user-submitted code.
// When creating a native contract (for anything other than a test case),
// care must be taken to ensure the following:
// (1) Determinism
// (2) Garbage collection (don't leak memory)
// (3) Memory safety (don't write code that accesses memory out of bounds)
// (4) Self-containment (which is another way of saying determinism; do not
// access memory outside of the contract's context, in a way that might have
// nondeterministic side effects. For example, do not store data in ways other
// than those provided by the syscall handler).
//
// These requirements need not be enforced within a test.
class NativeContract {
public:
  using function_t = std::function<InvokeStatus<int32_t>(SystemCallHandler &)>;

  NativeContract(std::unordered_map<MethodID, function_t> functions)
    : functions_(std::move(functions))
  {}

  NativeContract(NativeContract &&) = delete;
  NativeContract &operator=(NativeContract &&) = delete;
  NativeContract(NativeContract const &) = delete;
  NativeContract &operator=(NativeContract const &) = delete;

  InvokeStatus<int32_t> invoke(MethodID method,
                               SystemCallHandler &syscall_handler);

private:
  std::unordered_map<MethodID, function_t> functions_;
};

// Context that instantiates native contracts from a predefined set of
// contracts.
class NativeContext : public RuntimeContext {
public:
  NativeContext(std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
                    native_contracts)
    : contracts_(std::move(native_contracts))
  {}

  ~NativeContext() override = default;

  std::unique_ptr<Runtime> make_new_runtime(Contract const &contract,
                                            SystemCallHandler *syscall_handler,
                                            bool do_contract_verify) override;

  bool verify_contract(Contract const & /*unused*/) const override
  {
    return true;
  }

private:
  bool link_fn_nargs(std::string_view /*unused*/, std::string_view /*unused*/,
                     void * /*unused*/, uint8_t /*unused*/) override
  {
    return true;
  }

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contracts_;
};

// Wrapper around one native contract
class NativeRuntime : public Runtime {
public:
  // should be called only from within NativeContext::make_new_runtime
  NativeRuntime(NativeContract &contract,
                std::unique_ptr<HostCallContext> host_call_context);

  ~NativeRuntime() override = default;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override
  {
    return available_gas_;
  }
  void set_available_gas(uint64_t gas) noexcept override
  {
    available_gas_ = gas;
  }

  // No memory to access from within a native contract
  std::span<std::byte> get_memory() noexcept override { std::terminate(); }

private:
  InvokeStatus<int32_t> invoke_internal(std::string_view method_name) override;

  NativeContract &contract_;
  uint64_t available_gas_ = 0;
};

} // namespace hogd
