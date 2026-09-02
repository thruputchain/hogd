#pragma once

#include <algorithm>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace detail {

template<typename Expected, typename Actual>
struct TupleSerializesLike : std::false_type {};

template<typename... Es, typename... As>
requires (sizeof...(Es) == sizeof...(As))
struct TupleSerializesLike<std::tuple<Es...>, std::tuple<As...>>
  : std::bool_constant<(x23::serializes_like<As, Es> && ...)> {};

template<typename T> struct NativeContractArgs;

// Contract bodies are coroutines (they await syscalls), so they return
// x23::Async<InvokeStatus<R>> rather than InvokeStatus<R>.
template<typename R, typename... Args>
struct NativeContractArgs<
    x23::Async<InvokeStatus<R>> (*)(NativeSyscallContext &, Args...)> {
  using return_t = R;
  using args_t = std::tuple<Args...>;

  static auto extra_args(NativeSyscallContext &ctx)
  {
    return std::tuple<NativeSyscallContext &>{ctx};
  }
};

template<typename R, typename... Args>
struct NativeContractArgs<x23::Async<InvokeStatus<R>> (*)(Args...)> {
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
      return [](NativeSyscallContext &ctx,
                std::tuple_element_t<Is, args_t>... /*unused*/)
                 -> x23::Async<InvokeStatus<return_t>> {
        ctx.consume_gas(Gas);
        co_return InvokeError{InvokeErrorCode::Deterministic};
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
    static_assert(
        detail::TupleSerializesLike<expected_args_t, args_t>::value,
        "Argument mismatch (types must be same or serialize-equivalent)");

    using expected_return_t = meta_t::template return_t<Name>;
    if constexpr (std::is_void_v<return_t>) {
      static_assert(std::is_same_v<expected_return_t, std::tuple<>>,
                    "Return type mismatch");
    }
    else {
      static_assert(std::is_same_v<return_t, expected_return_t>,
                    "Return type mismatch");
    }

    // Key the native function table off the canonical method id (the generated
    // numeric `kEntrypoints` constant when available) rather than re-hashing
    // ordinary string literals, whose bytes depend on `-fexec-charset`. See
    // hogd issue #2579.
    Runtime::MethodID id = meta_t::template method_id<Name>();

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

    // TODO(geoff): figure out gas costs for native contracts like this
    constexpr static GasAmount kErrorGas = 0;

    // A coroutine, so not noexcept: NativeContract::invoke() re-establishes
    // that boundary around its frame allocation.
    auto wrapped_f =
        [f = std::forward<decltype(lambda)>(lambda)](
            NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
      try {
        // Mirror the sdk entrypoint glue: an args-decode failure (parse
        // error, trailing bytes) or unconsumed forwarded auth handles
        // (builder contracts take no Signer args) aborts the frame with
        // LibraryExn::Handling(DecodingArgs), byte-identical to a wasm
        // contract's wrapper — not a plain frame fault. See
        // default_wallet_parity_test.cc's malformed-calldata cases.
        auto decoding_args_abort = [&ctx]() -> InvokeStatus<void> {
          ctx.consume_gas(kErrorGas);
          LibraryExn exn(LibraryExn::kLibraryExnHandling);
          exn.handling() = HandlingError(HandlingError::kHandlingDecodingArgs);
          auto buf = x23::serialize_cxs(kLibraryExceptionSelector, exn);
          if (!buf) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          ctx.handler().set_return_buffer(ReturnBuffer(buf->begin(),
                                                       buf->end()),
                                          true, {});
          return {};
        };

        auto const &calldata = ctx.handler().get_calldata();
        auto args = Serializer<Format>::template deserialize<args_t>(
            std::as_bytes(std::span(calldata)));
        if (!args.has_value()) {
          co_return decoding_args_abort();
        }
        if (!ctx.handler().get_parent_supplied_auth_tokens().empty()) {
          co_return decoding_args_abort();
        }

        // Invoke the body and await it. extra_args is hoisted into this
        // frame because the body holds references into it across suspensions;
        // the inner lambda only forwards the call, so it is not itself a
        // coroutine.
        auto extra_args = detail::NativeContractArgs<fn_t>::extra_args(ctx);
        auto return_value =
            co_await [&]<size_t... Is>(std::index_sequence<Is...>)
            -> x23::Async<InvokeStatus<return_t>> {
          return [&]<size_t... ExtraIs>(std::index_sequence<ExtraIs...>) {
            return f(std::get<ExtraIs>(extra_args)..., std::get<Is>(*args)...);
          }(std::make_index_sequence<
                     std::tuple_size_v<decltype(extra_args)>>());
        }(std::make_index_sequence<std::tuple_size_v<args_t>>());
        if (!return_value.has_value()) {
          co_return return_value.error();
        }

        // Now we check whether the return value is a plain value,
        // or a variant of Result<T,E>

        auto do_return = [&ctx](auto const &r) -> InvokeStatus<void> {
          auto return_buffer = Serializer<Format>::serialize(r);
          if (!return_buffer.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          ctx.handler().set_return_buffer(std::move(*return_buffer), false, {});
          return {};
        };

        // Serialize error as (u64 selector, CXS body) and set as revert buffer.
        auto do_revert = [&ctx](auto const &err_value) -> InvokeStatus<void> {
          auto buf = err_value.serialize();
          if (!buf)
            return InvokeError{InvokeErrorCode::Deterministic};
          ctx.handler().set_return_buffer(std::move(*buf), true, {});
          return {};
        };

        if constexpr (is_contract_result_type<return_t>) {
          auto const &r = *return_value;

          if (r.is_ok()) {
            // void is so annoying
            if constexpr (std::is_same_v<decltype(r.ok()), void>) {
              co_return do_return(std::tuple<>{});
            }
            else {
              co_return do_return(r.ok());
            }
          }
          co_return do_revert(r.err());
        }
        else {
          // otherwise, return the received value
          if constexpr (std::is_same_v<decltype(*return_value), void>) {
            co_return do_return(std::tuple<>{});
          }
          else {
            co_return do_return(*return_value);
          }
        }
      } catch (...) {
        error("unrecoverable: unexpected exception in native contract");
        co_return InvokeError{InvokeErrorCode::Unrecoverable};
      }
    };

    auto [_, inserted] = functions_.emplace(id, std::move(wrapped_f));
    if (!inserted) {
      // Registration-time programming error: no untrusted input reaches here.
      panic("Method ID {} already exists", id);
    }
  }

  std::unique_ptr<NativeContract> build();

  template<is_contract_trait ContractTrait> bool check_methods()
  {
    // Check against the canonical method ids (see `add`), not ids re-hashed
    // from ordinary string literals; otherwise a divergent `-fexec-charset`
    // build would spuriously fail this check rather than expose the real
    // dispatch-key mismatch. See hogd issue #2579.
    if constexpr (requires { ContractTrait::kEntrypoints; }) {
      return std::ranges::all_of(ContractTrait::kEntrypoints,
                                 [this](Runtime::MethodID id) {
                                   return functions_.contains(id);
                                 });
    }
    else {
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
  }

  static InvokeError host_fn_error_conversion(HostFnError const &err)
  {
    switch (err.code()) {
    case HostFnErrorCode::OutOfGas:
      return InvokeError{InvokeErrorCode::OutOfGas};
    case HostFnErrorCode::Unrecoverable:
      return InvokeError{InvokeErrorCode::Unrecoverable};
    case HostFnErrorCode::Abandon:
      return InvokeError{InvokeErrorCode::Abandon};
    case HostFnErrorCode::ValidationFailure:
      return InvokeError{InvokeErrorCode::ValidationFailure};
    case HostFnErrorCode::Exit:
      return InvokeError{InvokeErrorCode::Deterministic};
    case HostFnErrorCode::Return:
      error("unrecoverable: contracts using the NativeContractBuilder should "
            "not call the Return syscall directly");
      return InvokeError{InvokeErrorCode::Unrecoverable};
    case HostFnErrorCode::NoneOrRecoverable:
      error("unrecoverable: host_fn_error_conversion: NoneOrRecoverable "
            "when error expected");
      return InvokeError{InvokeErrorCode::Unrecoverable};
    }
    error(
        "unrecoverable: host_fn_error_conversion: unknown HostFnErrorCode: {}",
        err.code());
    return InvokeError{InvokeErrorCode::Unrecoverable};
  }

private:
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> functions_;
  bool built_ = false;
};

} // namespace hogd
