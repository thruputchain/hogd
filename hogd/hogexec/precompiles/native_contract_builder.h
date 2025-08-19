#pragma once

#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

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

template<is_contract_trait ContractTrait, StringLiteral Name, GasAmount Gas>
struct AlwaysErrorMethod {
  using meta_t = ContractTraitMeta<ContractTrait>;
  using args_t = meta_t::template args_t<Name>;
  using return_t = meta_t::template return_t<Name>;

  constexpr static auto make_lambda()
  {
    return []<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
      return [](NativeRuntime &runtime,
                std::tuple_element_t<Is, args_t>... /*unused*/)
                 -> InvokeStatus<return_t> {
        if (!runtime.consume_gas(Gas)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }
        return InvokeError{InvokeErrorCode::kInvokeDeterministic};
      };
    }(std::make_index_sequence<std::tuple_size_v<args_t>>());
  }
};

} // namespace detail

template<is_contract_trait ContractTrait, StringLiteral Name, GasAmount Gas>
constexpr static auto kAlwaysErrorMethod =
    detail::AlwaysErrorMethod<ContractTrait, Name, Gas>::make_lambda();

class NativeContractBuilder {
public:
  constexpr static SerializationFormat kDefaultSerializationFormat =
      SerializationFormat::kTxs;

  // Asserts that the arguments to the supplied lambda match the spec, before
  // adding the lambda
  template<is_contract_trait ContractTrait, StringLiteral Name>
  void add(auto lambda)
  {
    using fn_t = decltype(&decltype(lambda)::operator());
    using args_t = typename detail::NativeContractArgs<fn_t>::args_t;
    using return_t = typename detail::NativeContractArgs<fn_t>::return_t;

    using meta_t = ContractTraitMeta<ContractTrait>;

    using expected_args_t = meta_t::template args_t<Name>;
    static_assert(std::is_same_v<args_t, expected_args_t>, "Argument mismatch");

    using expected_return_t = meta_t::template return_t<Name>;
    if constexpr (std::is_void_v<return_t>) {
      static_assert(std::is_same_v<expected_return_t, std::tuple<>>,
                    "Return type mismatch");
    }
    else {
      static_assert(std::is_same_v<return_t, expected_return_t>,
                    "Return type mismatch");
    }

    MethodID id = sdk_method_id(meta_t::template entrypoint<Name>());

    constexpr auto kFormat = meta_t::kSerializationFormat;
    add<kFormat>(id, std::forward<decltype(lambda)>(lambda));
  }

  template<SerializationFormat Format> void add(MethodID id, auto lambda)
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

      // Now we check whether the return value is a plain value,
      // or a variant of Result<T,E>
      constexpr static bool kIsResult =
          requires { return_t::kIsContractResultType == true; };

      auto do_return = [&handler](auto const &r,
                                  bool revert_frame) -> InvokeStatus<void> {
        auto return_buffer = Serializer<Format>::serialize(r);
        if (!return_buffer.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        handler.set_return_buffer(std::move(return_buffer.value()),
                                  revert_frame);
        return {};
      };

      if constexpr (kIsResult) {
        auto const &r = return_value.value();

        if (r.is_ok()) {
          // void is so annoying
          if constexpr (std::is_same_v<decltype(r.ok()), void>) {
            return do_return(std::tuple<>{}, false);
          }
          else {
            return do_return(r.ok(), false);
          }
        }
        if constexpr (std::is_same_v<decltype(r.err()), void>) {
          return do_return(std::tuple<>{}, true);
        }
        else {
          return do_return(r.err(), true);
        }
      }
      else {
        // otherwise, return the received value
        if constexpr (std::is_same_v<decltype(return_value.value()), void>) {
          return do_return(std::tuple<>{}, false);
        }
        else {
          return do_return(return_value.value(), false);
        }
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

} // namespace hogd
