#pragma once

#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

namespace detail {
template<typename T> struct NativeContractArgs;

template<typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (*)(NativeSyscallContext &,
                                              Args...)> {
  using return_t = R;
  using args_t = std::tuple<Args...>;

  static auto extra_args(NativeSyscallContext &ctx)
  {
    return std::tuple<NativeSyscallContext &>{ctx};
  }
};

template<typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (*)(NativeRuntime &, Args...)> {
  using return_t = R;
  using args_t = std::tuple<Args...>;
  static auto extra_args(NativeSyscallContext &ctx)
  {
    return std::tuple<NativeRuntime &>{ctx.runtime()};
  }
};

template<typename R, typename... Args>
struct NativeContractArgs<InvokeStatus<R> (*)(Args...)> {
  using return_t = R;
  using args_t = std::tuple<Args...>;

  static auto extra_args(NativeSyscallContext & /*unused*/)
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

  template<is_contract_trait ContractTrait, StringLiteral Name>
  void add(auto lambda)
  {
    add<ContractTrait, Name>(+lambda);
  }

  // Asserts that the arguments to the supplied lambda match the spec, before
  // adding the lambda
  template<is_contract_trait ContractTrait, StringLiteral Name, typename R,
           typename... Args>
  void add(R (*lambda)(Args...))
  {
    using fn_t = decltype(lambda);
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

    Runtime::MethodID id = sdk_method_id(meta_t::template entrypoint<Name>());

    constexpr auto kFormat = meta_t::kSerializationFormat;
    add<kFormat>(id, std::forward<decltype(lambda)>(lambda));
  }

  template<SerializationFormat Format>
  void add(Runtime::MethodID id, auto lambda)
  {
    add<Format>(id, +lambda);
  }

  template<SerializationFormat Format, typename R, typename... Args>
  void add(Runtime::MethodID id, R (*lambda)(Args...))
  {
    using fn_t = decltype(lambda);
    using args_t = typename detail::NativeContractArgs<fn_t>::args_t;
    using return_t = typename detail::NativeContractArgs<fn_t>::return_t;

    auto wrapped_f = [f = std::forward<decltype(lambda)>(lambda)](
                         NativeSyscallContext &ctx) -> InvokeStatus<void> {
      auto calldata = ctx.handler().get_calldata();
      auto args = Serializer<Format>::template deserialize<args_t>(
          std::as_bytes(std::span(calldata)));
      if (!args.has_value()) {
        return InvokeError{InvokeErrorCode::kInvokeDeterministic};
      }

      // invoke the lambda, and return the error if there is one
      auto return_value =
          [&]<size_t... Is>(
              std::index_sequence<Is...>) -> InvokeStatus<return_t> {
        auto extra_args = detail::NativeContractArgs<fn_t>::extra_args(ctx);
        return [&]<size_t... ExtraIs>(std::index_sequence<ExtraIs...>) {
          return f(std::get<ExtraIs>(extra_args)..., std::get<Is>(*args)...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(extra_args)>>());
      }(std::make_index_sequence<std::tuple_size_v<args_t>>());
      if (!return_value.has_value()) {
        return return_value.error();
      }

      // Now we check whether the return value is a plain value,
      // or a variant of Result<T,E>

      auto do_return = [&ctx](auto const &r,
                              bool revert_frame) -> InvokeStatus<void> {
        auto return_buffer = Serializer<Format>::serialize(r);
        if (!return_buffer.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        ctx.handler().set_return_buffer(std::move(return_buffer.value()),
                                        revert_frame);
        return {};
      };

      if constexpr (is_contract_result_type<return_t>) {
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

  template<is_contract_trait ContractTrait> bool check_methods()
  {
    for (size_t i = 0; i < ContractTrait::kExportNames.size(); ++i) {
      ContractEntrypointID entrypoint =
          sdk_entrypoint_derivation(ContractTrait::kTraitName,
                                    ContractTrait::kVersion,
                                    ContractTrait::kExportNames[i]);

      if (!functions_.contains(sdk_method_id(entrypoint))) {
        return false;
      }
    }
    return true;
  }

  static InvokeError host_fn_error_conversion(HostFnError const& err) {
    switch (err.code()) {
    case HostFnErrorCode::kHostOutOfGas:
      return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
    case HostFnErrorCode::kHostUnrecoverable:
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    case HostFnErrorCode::kHostAbortTransaction:
      return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
    case HostFnErrorCode::kHostValidationFailure:
      return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    case HostFnErrorCode::kHostExit:
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    case HostFnErrorCode::kHostReturn:
      error("unrecoverable: contracts using the NativeContractBuilder should not call the Return syscall directly");
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    case HostFnErrorCode::kHostNoneOrRecoverable:
      error("unrecoverable: host_fn_error_conversion: kHostNoneOrRecoverable when error expected");
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    }
    error("unrecoverable: host_fn_error_conversion: unknown HostFnErrorCode: {}", err.code());
    return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
  }

private:
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> functions_;
  bool built_ = false;
};

} // namespace hogd
