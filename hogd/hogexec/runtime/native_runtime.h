#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "transaction.hh"
#include "types.hh"

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
class NativeContract {
public:
  using function_t =
      std::function<InvokeStatus<void>(SystemCallHandler &, NativeRuntime &)>;

  NativeContract(std::unordered_map<MethodID, function_t> functions)
    : functions_(std::move(functions))
  {}

  NativeContract(NativeContract &&) = delete;
  NativeContract &operator=(NativeContract &&) = delete;
  NativeContract(NativeContract const &) = delete;
  NativeContract &operator=(NativeContract const &) = delete;

  InvokeStatus<void> invoke(MethodID method, SystemCallHandler &syscall_handler,
                            NativeRuntime &runtime);

private:
  std::unordered_map<MethodID, function_t> functions_;
};

namespace detail {
template<typename T> struct NativeContractArgs;

template<typename T, typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (
    T::*)(SystemCallHandler &, NativeRuntime &, Args...) const> {
  using return_t = R;
  using args_t = std::tuple<Args...>;

  static auto extra_args(SystemCallHandler &handler, NativeRuntime &runtime)
  {
    return std::tuple<SystemCallHandler &, NativeRuntime &>{handler, runtime};
  }
};

template<typename T, typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (T::*)(SystemCallHandler &, Args...)
                              const> {
  using return_t = R;
  using args_t = std::tuple<Args...>;
  static auto extra_args(SystemCallHandler &handler, NativeRuntime & /*unused*/)
  {
    return std::tuple<SystemCallHandler &>{handler};
  }
};

template<typename T, typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (T::*)(NativeRuntime &, Args...)
                              const> {
  using return_t = R;
  using args_t = std::tuple<Args...>;
  static auto extra_args(SystemCallHandler & /*unused*/, NativeRuntime &runtime)
  {
    return std::tuple<NativeRuntime &>{runtime};
  }
};

template<typename T, typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (T::*)(Args...) const> {
  using return_t = R;
  using args_t = std::tuple<Args...>;

  static auto extra_args(SystemCallHandler & /*unused*/,
                         NativeRuntime & /*unused*/)
  {
    return std::tuple{};
  }
};

} // namespace detail

class NativeContractBuilder {
public:
  constexpr static SerializationFormat kDefaultSerializationFormat =
      SerializationFormat::kTxs;

  template<SerializationFormat Format = kDefaultSerializationFormat>
  void add(MethodID id, auto lambda)
  {
    using fn_t = decltype(&decltype(lambda)::operator());
    using args_t = typename detail::NativeContractArgs<fn_t>::args_t;
    using return_t = typename detail::NativeContractArgs<fn_t>::return_t;

    auto wrapped_f = [f = std::forward<decltype(lambda)>(lambda)](
                         SystemCallHandler &handler,
                         NativeRuntime &runtime) -> InvokeStatus<void> {
      auto calldata = handler.get_calldata();
      auto args = Serializer<Format>::template deserialize<args_t>(
          std::as_bytes(std::span(calldata)));
      if (!args.has_value()) {
        return InvokeError{InvokeErrorCode::kInvokeDeterministic};
      }

      // invoke the lambda, and return the error if there is one
      auto return_value =
          [&]<size_t... Is>(
              std::index_sequence<Is...>) -> InvokeStatus<return_t> {
        auto extra_args =
            detail::NativeContractArgs<fn_t>::extra_args(handler, runtime);
        return [&]<size_t... ExtraIs>(std::index_sequence<ExtraIs...>) {
          return f(std::get<ExtraIs>(extra_args)..., std::get<Is>(*args)...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(extra_args)>>());
      }(std::make_index_sequence<std::tuple_size_v<args_t>>());
      if (!return_value.has_value()) {
        return return_value.error();
      }

      // otherwise, return the received value
      if constexpr (!std::is_same_v<return_t, void>) {
        auto return_buffer =
            Serializer<Format>::serialize(return_value.value());
        if (!return_buffer.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        handler.set_return_buffer(std::move(return_buffer.value()));
        return {};
      }
      else {
        handler.set_return_buffer(ReturnBuffer{});
        return {};
      }
    };

    auto [_, inserted] = functions_.emplace(id, std::move(wrapped_f));
    if (!inserted) {
      throw std::runtime_error("Method ID already exists");
    }
  }

  std::unique_ptr<NativeContract> build();

private:
  std::unordered_map<MethodID, NativeContract::function_t> functions_;
  bool built_ = false;
};

// Context that can instantiate native contracts from a predefined set of
// contracts.
class NativeContext : public RuntimeContext {
public:
  NativeContext(std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
                    native_contracts)
    : contracts_(std::move(native_contracts))
  {}

  ~NativeContext() override = default;

  std::unique_ptr<Runtime> make_new_runtime(Contract const &contract,
                                            SHA256Hash const &hash,
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
  InvokeStatus<void> invoke_internal(std::string_view method_name) override;

  NativeContract &contract_;
  uint64_t available_gas_ = 0;
};

} // namespace hogd
