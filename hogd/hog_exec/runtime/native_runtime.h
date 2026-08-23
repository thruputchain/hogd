#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/unique_function.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "x23/async.h"

namespace hogd {

struct Contract;
class SystemCallHandler;
class NativeRuntime;

// One native contract.  Can be invoked with a method id and calldata,
// just like a regular contract.
//
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
//
// Unless there is a compelling reason to do so,
// each native contract method (the lambda) should be stateless, and,
// like a regular contract, only manage persistent state via the
// SystemCallHandler interface.
//
// Native contracts do not come with metering built in.  Each native contract
// method should therefore make gas metering calls (consume_gas) to the supplied
// runtime pointer, as necessary.
class NativeSyscallContext;

class NativeContract {
public:
  // Not noexcept: these bodies are coroutines now (they await syscalls), and
  // a coroutine's frame allocation can throw before the body runs.
  // NativeContract::invoke() re-establishes the noexcept boundary.
  using function_t = UniqueFunction<x23::Async<InvokeStatus<void>>(
      NativeSyscallContext &) const>;
  using function_entry_t = std::pair<Runtime::MethodID, function_t>;

  NativeContract(std::unordered_map<Runtime::MethodID, function_t> functions)
    : functions_([&] {
        std::vector<function_entry_t> v(
            std::make_move_iterator(functions.begin()),
            std::make_move_iterator(functions.end()));
        std::ranges::sort(v, [](auto const &a, auto const &b) {
          return a.first < b.first;
        });
        return v;
      }())
  {}

  NativeContract(NativeContract &&) = delete;
  NativeContract &operator=(NativeContract &&) = delete;
  NativeContract(NativeContract const &) = delete;
  NativeContract &operator=(NativeContract const &) = delete;

  x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
  invoke(Runtime::MethodID method, SystemCallHandler &syscall_handler,
         uint64_t gas_limit) const;

private:
  const function_t *find_function(Runtime::MethodID method) const;

  // Body of invoke(); owns the NativeSyscallContext across suspensions.
  // Static: it works entirely from the resolved function and the handler.
  static x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_impl(function_t const &fn, SystemCallHandler &syscall_handler,
              uint64_t gas_limit);

  const std::vector<function_entry_t> functions_;
};

// Context that can instantiate native contracts from a predefined set of
// contracts.
class NativeContext : public RuntimeContext {
public:
  NativeContext(std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
                    native_contracts)
    : contracts_(std::move(native_contracts))
  {}

  ~NativeContext() override = default;

  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override
  {
    CHECK(contract.contract_type() == ContractType::kNative);
    if (contracts_.contains(
            std::to_underlying(contract.native_contract_id()))) {
      return InvokeErrorCode::None;
    }
    return InvokeErrorCode::Deterministic;
  }

private:
  const std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contracts_;
};

// Wrapper around one instantiation of a native contract.
//
// Note that creating multiple copies of a native contract
// (e.g. within one transaction) does NOT duplicate memory,
// or implement any kind of state reversion other than the
// state managed by the SystemCallHandler (so if at all possible,
// all state managed by a native contract should be mediated by
// the SystemCallHandler).
//
// Does not, by default, implement any kind of gas metering.
// As necessary, the methods inside the NativeContract
// should implement their own metering.
class NativeRuntime : public Runtime {
public:
  // should be called only from within NativeContext::make_new_runtime
  NativeRuntime(NativeContract &contract, SystemCallHandler *syscall_handler);

  ~NativeRuntime() override = default;

private:
  x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;

  NativeContract &contract_;
};

} // namespace hogd
