#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/ffi_helpers.h"
#include "hogd/hog_exec/components/ffi_types.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/invocation_frame.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

using default_wallet::WalletCall;
using default_wallet::WalletError;
using default_wallet::WalletTokenCall;
using default_wallet::WalletTokenRef;
using default_wallet::WalletTokenSource;

// Result types (and the WalletError exception selector inside them) come
// straight from the generated trait in built_contracts/default_wallet.h.
using wallet_meta = ContractTraitMeta<default_wallet::DefaultWallet>;
using execute_result_t = wallet_meta::return_t<u8"execute">;
using deploy_result_t = wallet_meta::return_t<u8"deploy">;

template<typename> struct result_exception;
template<typename T, typename E> struct result_exception<Result<T, E>> {
  using type = E;
};
using WalletException = result_exception<execute_result_t>::type;

using wallet_bytes_vec_t = execute_result_t::ok_type;
using wallet_bytes_t = wallet_bytes_vec_t::value_type;

// execute_with_tokens returns and throws exactly what execute does, so the
// two share their result type and their return accounting.
static_assert(
    std::is_same_v<wallet_meta::return_t<u8"execute_with_tokens">,
                   execute_result_t>);
using token_execute_args_t = wallet_meta::args_t<u8"execute_with_tokens">;
using wallet_mints_t = std::tuple_element_t<0, token_execute_args_t>;
using wallet_token_calls_t = std::tuple_element_t<1, token_execute_args_t>;

// Reference constants (implementation.rs).
constexpr size_t kMaxCalls = 16;
// Mirrors implementation.rs, which compiles its own limit in, so this
// follows that contract rather than kConfigParamMaxReturnBufferLen.
constexpr size_t kMaxReturnDataBytes = 1024;

// Mirrors require_root_invocation(): GetMsgSender must fail with exactly
// IsRootTransaction. Returns whether this is a root invocation; host-level
// errors propagate.
x23::Async<InvokeStatus<bool>>
is_root_invocation(NativeSyscallContext &ctx)
{
  ContractAddress sender{};
  auto r = co_await ctx.syscall<SyscallNo::kGetMsgSender>(sender);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  co_return *r == SyscallErrorCode::kSyscallErrorCodeIsRootTransaction;
}

// Mirrors require_pubkey_signature_auth(): the auth claim must be a
// kPubkeySignature whose derived address equals the current address.
// Any syscall error code, other claim type, or mismatch -> false
// (InvalidAuthClaim in the caller); host-level errors propagate.
x23::Async<InvokeStatus<bool>>
has_valid_pubkey_auth(NativeSyscallContext &ctx)
{
  generated::AuthClaim ffi_claim{};
  auto r = co_await ctx.syscall<SyscallNo::kGetAuthClaim>(ffi_claim);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return false;
  }
  if (ffi_claim.tag() != generated::AuthClaim::AuthClaim_kPubkeySignature) {
    co_return false;
  }
  auto preimage = FFIHelperType<generated::PubkeyAccountPreimage>::to_cpp(
      ffi_claim.k_pubkey_signature.pubkey_preimage);
  if (!preimage) {
    co_return false;
  }
  auto derived = pubkey_auth_address(*preimage);
  if (!derived) {
    co_return false;
  }
  co_return *derived == ctx.handler().get_current_address();
}

// Shared auth preamble; nullopt = auth passed, otherwise the WalletError to
// revert with. Host-level errors propagate.
x23::Async<InvokeStatus<std::optional<WalletError>>>
check_root_pubkey_auth(NativeSyscallContext &ctx)
{
  auto root = co_await is_root_invocation(ctx);
  if (!root.has_value()) {
    co_return root.error();
  }
  // Root-only reachability is enforced by resolution: the wallet has no
  // deployment row, its contract hash is banned from kDeployContract, and
  // resolve_runtime only falls back to it on an empty frame stack. A nested
  // wallet frame is handler-level invariant breakage.
  CHECK(*root);
  auto auth = co_await has_valid_pubkey_auth(ctx);
  if (!auth.has_value()) {
    co_return auth.error();
  }
  if (!*auth) {
    co_return std::optional{WalletError::InvalidAuthClaim};
  }
  co_return std::optional<WalletError>{};
}

using child_result_t = Result<std::vector<uint8_t>, WalletException>;

// Execute and deployment initialization share child-frame status handling.
// `auth_tokens` are forwarded to the child. `returned_tokens`, when given,
// receives the ids the child's returned tokens took in this frame; it is
// written only when the child returns.
x23::Async<InvokeStatus<child_result_t>>
invoke_child(NativeSyscallContext &ctx, ContractAddress const &target,
             ContractEntrypointID const &entrypoint, uint64_t gas_limit,
             wallet_bytes_t const &calldata,
             std::span<uint32_t> auth_tokens = {},
             AuthTokenIdRange *returned_tokens = nullptr)
{
  CopyableInvokeInputs inputs{
      .entrypoint_id = sdk_method_id(entrypoint),
      .gas_limit = gas_limit,
      .flags = std::to_underlying(InvokeFlag::None),
  };
  CopyableInvokeOutputs outputs{};
  auto inv = co_await ctx.syscall<SyscallNo::kInvoke>(
      target, std::as_bytes(std::span(calldata)), auth_tokens, inputs,
      outputs);
  if (!inv.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(inv.error());
  }
  if (*inv != SyscallErrorCode::kSyscallErrorCodeNone) {
    // Rejected before a child frame ran: sdk contract_call Err(_).
    co_return child_result_t::make_err(
        WalletException{WalletError::CallFaulted});
  }
  if (outputs.frame_status != InvocationFrameStatusCode::Return &&
      outputs.frame_status != InvocationFrameStatusCode::Abort) {
    // Fault or unexpected status: sdk contract_call Err(Fault).
    co_return child_result_t::make_err(
        WalletException{WalletError::CallFaulted});
  }

  // Fetch the child's return or abort payload as the SDK does.
  std::vector<uint8_t> return_buf(outputs.return_buffer_len);
  RuntimeMemoryLength return_written = 0;
  uint32_t returned_auth_token_start = 0;
  uint32_t returned_auth_token_count = 0;
  auto get = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
      std::as_writable_bytes(std::span(return_buf)), return_written,
      returned_auth_token_start, returned_auth_token_count);
  if (!get.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(get.error());
  }
  // A fetch error or short write maps to CallFaulted, matching the SDK.
  if (*get != SyscallErrorCode::kSyscallErrorCodeNone ||
      return_written != return_buf.size()) {
    co_return child_result_t::make_err(
        WalletException{WalletError::CallFaulted});
  }

  if (outputs.frame_status == InvocationFrameStatusCode::Abort) {
    // SDK RawExceptionRef only requires a CXS u64 selector; its body is
    // opaque. Preserve the entire wire buffer, including unknown exceptions.
    uint64_t selector = 0;
    if (!x23::deserialize_cxs_from(&selector, x23::BufViewEnd{return_buf})) {
      co_return child_result_t::make_err(
          WalletException{WalletError::CallAborted});
    }
    co_return child_result_t::make_err(child_result_t::err_type::from_raw_abort(
        ReturnBuffer(return_buf.begin(), return_buf.end())));
  }

  if (returned_tokens != nullptr) {
    *returned_tokens = {.start = returned_auth_token_start,
                        .count = returned_auth_token_count};
  }
  co_return child_result_t::make_ok(std::move(return_buf));
}

// Mirrors require_call_count(); nullopt = the count is acceptable.
std::optional<WalletError>
check_call_count(size_t call_count)
{
  if (call_count == 0) {
    return WalletError::EmptyCallList;
  }
  if (call_count > kMaxCalls) {
    return WalletError::TooManyCalls;
  }
  return std::nullopt;
}

// Mirrors BatchReturns: the batch's return value as it accumulates, held to
// the return-data limit after every call.
class BatchReturns {
public:
  // Running CXS size of the eventual Vec<WalletBytes> return: the outer
  // length prefix now, each item's prefix + bytes as it lands. All operands
  // are bounded (<= 16 calls, <= 1024-byte returns), so the reference's
  // u32/checked_add overflow guards are unreachable here and elided.
  explicit BatchReturns(size_t call_count)
    : encoded_len_(x23::cxs_size(static_cast<uint32_t>(call_count)))
  {}

  // False when `return_buf` takes the batch past the limit.
  [[nodiscard]] bool push(std::vector<uint8_t> const &return_buf)
  {
    encoded_len_ += x23::cxs_size(static_cast<uint32_t>(return_buf.size())) +
                    return_buf.size();
    if (encoded_len_ > kMaxReturnDataBytes) {
      return false;
    }
    wallet_bytes_t item;
    item.append_range(return_buf);
    returns_.push_back(std::move(item));
    return true;
  }

  wallet_bytes_vec_t take() && { return std::move(returns_); }

private:
  wallet_bytes_vec_t returns_;
  size_t encoded_len_;
};

// Mirrors BatchTokens: the auth tokens a batch holds, by where each came
// from.
class BatchTokens {
public:
  void add_minted(uint32_t token_id) { minted_.push_back(token_id); }
  void add_returned(AuthTokenIdRange tokens) { returned_.push_back(tokens); }

  // Resolves `token` to the id of a token the wallet still holds and marks it
  // forwarded, or returns the WalletError the reference throws.
  std::variant<uint32_t, WalletError> take(WalletTokenRef const &token)
  {
    std::optional<uint32_t> token_id;
    switch (token.source) {
    case WalletTokenSource::Minted:
      if (token.call == 0 && token.index < minted_.size()) {
        token_id = minted_[token.index];
      }
      break;
    case WalletTokenSource::Returned:
      // Only completed calls have an entry, so a reference to the current or
      // a later call finds nothing.
      if (token.call < returned_.size() &&
          token.index < returned_[token.call].count) {
        // Cannot overflow: the runtime assigned the whole range.
        token_id = returned_[token.call].start + token.index;
      }
      break;
    }
    if (!token_id) {
      return WalletError::InvalidTokenRef;
    }
    if (std::ranges::contains(forwarded_, *token_id)) {
      return WalletError::TokenAlreadyForwarded;
    }
    forwarded_.push_back(*token_id);
    return *token_id;
  }

private:
  std::vector<uint32_t> minted_;
  // Per completed call, the ids of the tokens it returned.
  std::vector<AuthTokenIdRange> returned_;
  // Every id forwarded so far. A successful call takes the tokens it was
  // forwarded and a failed one ends the batch, so none can go out twice.
  std::vector<uint32_t> forwarded_;
};

// Mirrors thruput_sdk::block::get_block_height(): the height, or the syscall
// exception the SDK throws in its place.
x23::Async<InvokeStatus<std::variant<BlockHeight, LibraryException>>>
get_block_height(NativeSyscallContext &ctx)
{
  using result_t = std::variant<BlockHeight, LibraryException>;
  BlockHeight height{};
  auto r = co_await ctx.syscall<SyscallNo::kGetBlockHeight>(height);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return result_t{
        NativeContractBuilder::syscall_exception(SyscallNo::kGetBlockHeight,
                                                 *r)};
  }
  co_return result_t{height};
}

x23::Async<InvokeStatus<deploy_result_t>>
deploy_contract(NativeSyscallContext &ctx, SHA256Hash const &nonce,
                SHA256Hash const &contract_hash)
{
  ContractAddress deployed{};
  auto r =
      co_await ctx.syscall<SyscallNo::kDeployContract>(nonce, contract_hash,
                                                       deployed);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return deploy_result_t::make_err(
        NativeContractBuilder::syscall_exception(SyscallNo::kDeployContract,
                                                 *r));
  }
  co_return deploy_result_t::make_ok(deployed);
}

} // namespace

std::unique_ptr<NativeContract>
make_native_default_wallet_contract()
{
  NativeContractBuilder builder;

  builder.add<default_wallet::DefaultWallet, u8"execute">(
      // The builder decays this lambda to a function pointer and deduces the
      // deserialized argument tuple from its parameter types, so `calls` must
      // be taken by value (a reference would not satisfy serializes_like).
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      [](NativeSyscallContext &ctx, x23::Vec<WalletCall, 4294967295> calls)
          -> x23::Async<InvokeStatus<execute_result_t>> {
        auto auth_err = co_await check_root_pubkey_auth(ctx);
        if (!auth_err.has_value()) {
          co_return auth_err.error();
        }
        if (auth_err->has_value()) {
          co_return execute_result_t::make_err(WalletException{**auth_err});
        }

        if (auto count_err = check_call_count(calls.size())) {
          co_return execute_result_t::make_err(WalletException{*count_err});
        }

        BatchReturns returns(calls.size());
        for (auto const &call : calls) {
          auto child = co_await invoke_child(ctx, call.target, call.entrypoint,
                                             call.gas_limit, call.calldata);
          if (!child.has_value()) {
            co_return child.error();
          }
          if (!child->is_ok()) {
            co_return execute_result_t::make_err(child->err());
          }
          if (!returns.push(child->ok())) {
            co_return execute_result_t::make_err(
                WalletException{WalletError::ReturnDataTooLarge});
          }
        }

        co_return execute_result_t::make_ok(std::move(returns).take());
      });

  builder.add<default_wallet::DefaultWallet, u8"execute_with_tokens">(
      // The builder requires owned arguments for its deserialized tuple.
      // NOLINTBEGIN(performance-unnecessary-value-param)
      [](NativeSyscallContext &ctx, wallet_mints_t mints,
         wallet_token_calls_t calls)
          -> x23::Async<InvokeStatus<execute_result_t>> {
        // NOLINTEND(performance-unnecessary-value-param)
        auto auth_err = co_await check_root_pubkey_auth(ctx);
        if (!auth_err.has_value()) {
          co_return auth_err.error();
        }
        if (auth_err->has_value()) {
          co_return execute_result_t::make_err(WalletException{**auth_err});
        }

        if (auto count_err = check_call_count(calls.size())) {
          co_return execute_result_t::make_err(WalletException{*count_err});
        }

        // Minted before any call runs, so no callee's behavior can change
        // which token a Minted reference names. The tokens are forwarded
        // within this transaction, so they need not outlive the block.
        auto expiry = co_await get_block_height(ctx);
        if (!expiry.has_value()) {
          co_return expiry.error();
        }
        if (auto *exn = std::get_if<LibraryException>(&*expiry)) {
          co_return execute_result_t::make_err(std::move(*exn));
        }
        BatchTokens tokens;
        for (auto const &node : mints) {
          uint32_t token_id = 0;
          auto r = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              std::get<BlockHeight>(*expiry), std::as_bytes(std::span(node)),
              token_id);
          if (!r.has_value()) {
            co_return NativeContractBuilder::host_fn_error_conversion(
                r.error());
          }
          if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return execute_result_t::make_err(
                NativeContractBuilder::syscall_exception(
                    SyscallNo::kMintAuthToken, *r));
          }
          tokens.add_minted(token_id);
        }

        BatchReturns returns(calls.size());
        for (auto const &call : calls) {
          std::vector<uint32_t> auth_tokens;
          auth_tokens.reserve(call.tokens.size());
          for (auto const &token : call.tokens) {
            auto taken = tokens.take(token);
            if (auto *err = std::get_if<WalletError>(&taken)) {
              co_return execute_result_t::make_err(WalletException{*err});
            }
            auth_tokens.push_back(std::get<uint32_t>(taken));
          }

          AuthTokenIdRange returned_tokens;
          auto child = co_await invoke_child(ctx, call.target, call.entrypoint,
                                             call.gas_limit, call.calldata,
                                             auth_tokens, &returned_tokens);
          if (!child.has_value()) {
            co_return child.error();
          }
          if (!child->is_ok()) {
            co_return execute_result_t::make_err(child->err());
          }
          tokens.add_returned(returned_tokens);
          if (!returns.push(child->ok())) {
            co_return execute_result_t::make_err(
                WalletException{WalletError::ReturnDataTooLarge});
          }
        }

        co_return execute_result_t::make_ok(std::move(returns).take());
      });

  builder.add<default_wallet::DefaultWallet, u8"deploy">(
      // The builder requires owned arguments for its deserialized tuple.
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      [](NativeSyscallContext &ctx, SHA256Hash nonce, SHA256Hash contract_hash,
         std::tuple<int64_t, uint64_t> amount,
         x23::Ptr<default_wallet::WalletInitCall> initializer)
          -> x23::Async<InvokeStatus<deploy_result_t>> {
        auto auth_err = co_await check_root_pubkey_auth(ctx);
        if (!auth_err.has_value()) {
          co_return auth_err.error();
        }
        if (auth_err->has_value()) {
          co_return deploy_result_t::make_err(WalletException{**auth_err});
        }

        if (amount != std::tuple<int64_t, uint64_t>{}) {
          ContractAddress target{};
          auto r = co_await ctx.syscall<SyscallNo::kGetDeployAddress>(
              static_cast<ContractAddress const *>(nullptr), nonce,
              contract_hash, target);
          if (!r.has_value()) {
            co_return NativeContractBuilder::host_fn_error_conversion(
                r.error());
          }
          if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return deploy_result_t::make_err(
                NativeContractBuilder::syscall_exception(
                    SyscallNo::kGetDeployAddress, *r));
          }
          // Invoke the ordinary token contract to preserve validation, caller
          // identity and events. Both effects remain inside the wallet root
          // frame.
          ContractTraitClient<itc_erc20::ERC20> token(
              kNativeTokenContractAddress);
          auto transfer =
              co_await token.invoke_opt<u8"transfer">(ctx, target, amount);
          if (!transfer.has_value()) {
            co_return NativeContractBuilder::host_fn_error_conversion(
                transfer.error());
          }
          if (!transfer->has_value() || !transfer->value().has_value()) {
            co_return deploy_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }
          if (!transfer->value()->is_ok()) {
            auto abort = transfer->value()->err().serialize();
            if (!abort) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
            co_return deploy_result_t::make_err(
                deploy_result_t::err_type::from_raw_abort(std::move(*abort)));
          }
        }
        auto deployed = co_await deploy_contract(ctx, nonce, contract_hash);
        if (!deployed.has_value() || !deployed->is_ok()) {
          co_return deployed;
        }
        if (initializer) {
          auto child = co_await invoke_child(ctx, deployed->ok(),
                                             initializer->entrypoint,
                                             initializer->gas_limit,
                                             initializer->calldata);
          if (!child.has_value()) {
            co_return child.error();
          }
          if (!child->is_ok()) {
            co_return deploy_result_t::make_err(child->err());
          }
        }
        co_return deployed;
      });

  assert(builder.check_methods<default_wallet::DefaultWallet>());
  return builder.build();
}

} // namespace hogd
