#include "hogd/hogexec/precompiles/precompile_default_wallet.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "built_contracts/default_wallet.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/contract_result.h"
#include "hogd/hogexec/components/ffi_helpers.h"
#include "hogd/hogexec/components/ffi_types.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

using default_wallet::WalletCall;
using default_wallet::WalletError;

// Selector for WalletError exceptions; must match the generated
// export_types in built_contracts/default_wallet.h.
constexpr uint64_t kWalletErrorSelector = 0x931d3297d37d1bdfULL;
using WalletException = ContractException<WalletError, kWalletErrorSelector>;

// Mirror the trait's generated types exactly (the builder static_asserts
// the lambda signatures against export_types, so a drift fails to compile).
using wallet_bytes_t = x23::Vec<uint8_t, 4294967295>;
using wallet_bytes_vec_t = x23::Vec<wallet_bytes_t, 4294967295>;
using execute_result_t = Result<wallet_bytes_vec_t, WalletException>;
using deploy_result_t = Result<ContractAddress, WalletException>;

// Reference constants (implementation.rs).
constexpr size_t kMaxCalls = 16;
constexpr size_t kMaxReturnDataBytes = kMaxReturnBufferLen; // 1024

// Reference cxs_u32_len: the CXS length-prefix width for a u32 value.
// None (nullopt) mirrors the Rust u32::try_from failure.
std::optional<size_t>
cxs_u32_len(size_t value)
{
  if (value > UINT32_MAX) {
    return std::nullopt;
  }
  if (value <= 62) {
    return 1;
  }
  if (value <= 255) {
    return 2;
  }
  if (value <= 65535) {
    return 3;
  }
  return 5;
}

std::optional<size_t>
encoded_return_item_len(size_t return_data_len)
{
  auto prefix = cxs_u32_len(return_data_len);
  if (!prefix) {
    return std::nullopt;
  }
  // Cannot overflow size_t at these magnitudes, but mirror the reference's
  // checked_add anyway.
  if (*prefix > SIZE_MAX - return_data_len) {
    return std::nullopt;
  }
  return *prefix + return_data_len;
}

// Mirrors require_root_invocation(): GetMsgSender must fail with exactly
// IsRootTransaction. Returns whether this is a root invocation; host-level
// errors propagate.
InvokeStatus<bool>
is_root_invocation(NativeSyscallContext &ctx)
{
  ContractAddress sender{};
  auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  return *r == SyscallErrorCode::kSyscallErrorCodeIsRootTransaction;
}

// Mirrors require_pubkey_signature_auth(): the auth claim must be a
// kPubkeySignature whose derived address equals the current address.
// Any syscall error code, other claim type, or mismatch -> false
// (InvalidAuthClaim in the caller); host-level errors propagate.
InvokeStatus<bool>
has_valid_pubkey_auth(NativeSyscallContext &ctx)
{
  generated::AuthClaim ffi_claim{};
  auto r = ctx.syscall<SyscallNo::kGetAuthClaim>(ffi_claim);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return false;
  }
  if (ffi_claim.tag() != generated::AuthClaim::AuthClaim_kPubkeySignature) {
    return false;
  }
  auto preimage = FFIHelperType<generated::PubkeyAccountPreimage>::to_cpp(
      ffi_claim.k_pubkey_signature.pubkey_preimage);
  if (!preimage) {
    return false;
  }
  auto derived = pubkey_auth_address(*preimage);
  if (!derived) {
    return false;
  }
  return *derived == ctx.handler().get_current_address();
}

// Shared auth preamble; nullopt = auth passed, otherwise the WalletError to
// revert with. Host-level errors propagate.
InvokeStatus<std::optional<WalletError>>
check_root_pubkey_auth(NativeSyscallContext &ctx)
{
  auto root = is_root_invocation(ctx);
  if (!root.has_value()) {
    return root.error();
  }
  if (!*root) {
    return std::optional{WalletError::NotRootInvocation};
  }
  auto auth = has_valid_pubkey_auth(ctx);
  if (!auth.has_value()) {
    return auth.error();
  }
  if (!*auth) {
    return std::optional{WalletError::InvalidAuthClaim};
  }
  return std::optional<WalletError>{};
}

} // namespace

std::unique_ptr<NativeContract>
make_native_default_wallet_contract()
{
  NativeContractBuilder builder;

  builder.add<default_wallet::DefaultWallet, u8"execute">(
      [](NativeSyscallContext &ctx,
         x23::Vec<WalletCall, 4294967295> calls)
          -> InvokeStatus<execute_result_t> {
        auto auth_err = check_root_pubkey_auth(ctx);
        if (!auth_err.has_value()) {
          return auth_err.error();
        }
        if (auth_err->has_value()) {
          return execute_result_t::make_err(WalletException{**auth_err});
        }

        if (calls.empty()) {
          return execute_result_t::make_err(
              WalletException{WalletError::EmptyCallList});
        }
        if (calls.size() > kMaxCalls) {
          return execute_result_t::make_err(
              WalletException{WalletError::TooManyCalls});
        }

        wallet_bytes_vec_t returns;
        auto total_len = cxs_u32_len(calls.size());
        if (!total_len) {
          return execute_result_t::make_err(
              WalletException{WalletError::ReturnDataTooLarge});
        }
        size_t encoded_return_data_len = *total_len;

        for (auto const &call : calls) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = sdk_method_id(call.entrypoint),
              .gas_limit = call.gas_limit,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};
          auto inv = ctx.syscall<SyscallNo::kInvoke>(
              call.target, std::as_bytes(std::span(call.calldata)),
              /*auth tokens*/ std::span<uint32_t>{}, inputs, outputs);
          if (!inv.has_value()) {
            return NativeContractBuilder::host_fn_error_conversion(
                inv.error());
          }
          if (*inv != SyscallErrorCode::kSyscallErrorCodeNone) {
            // Rejected before a child frame ran: sdk contract_call Err(_).
            return execute_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }
          if (outputs.frame_status == InvocationFrameStatusCode::Abort) {
            return execute_result_t::make_err(
                WalletException{WalletError::CallAborted});
          }
          if (outputs.frame_status != InvocationFrameStatusCode::Return) {
            // Fault or unexpected status: sdk contract_call Err(Fault).
            return execute_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }

          // Fetch the child's return bytes (the sdk fetches — and drops —
          // returned auth-token ids too; mirror the fetch).
          std::vector<std::byte> return_buf(outputs.return_buffer_len);
          RuntimeMemoryLength return_written = 0;
          std::vector<std::byte> auth_buf(outputs.returned_auth_token_len);
          RuntimeMemoryLength auth_written = 0;
          auto get = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
              std::span<std::byte>(return_buf), return_written,
              std::span<std::byte>(auth_buf), auth_written);
          if (!get.has_value()) {
            return NativeContractBuilder::host_fn_error_conversion(
                get.error());
          }
          if (*get != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto item_len = encoded_return_item_len(return_buf.size());
          if (!item_len ||
              *item_len > SIZE_MAX - encoded_return_data_len) {
            return execute_result_t::make_err(
                WalletException{WalletError::ReturnDataTooLarge});
          }
          encoded_return_data_len += *item_len;
          if (encoded_return_data_len > kMaxReturnDataBytes) {
            return execute_result_t::make_err(
                WalletException{WalletError::ReturnDataTooLarge});
          }

          wallet_bytes_t item;
          item.reserve(return_buf.size());
          for (auto b : return_buf) {
            item.push_back(static_cast<uint8_t>(b));
          }
          returns.push_back(std::move(item));
        }

        return execute_result_t::make_ok(std::move(returns));
      });

  builder.add<default_wallet::DefaultWallet, u8"deploy">(
      [](NativeSyscallContext &ctx, SHA256Hash nonce,
         SHA256Hash contract_hash) -> InvokeStatus<deploy_result_t> {
        auto auth_err = check_root_pubkey_auth(ctx);
        if (!auth_err.has_value()) {
          return auth_err.error();
        }
        if (auth_err->has_value()) {
          return deploy_result_t::make_err(WalletException{**auth_err});
        }

        ContractAddress deployed{};
        auto r = ctx.syscall<SyscallNo::kDeployContract>(nonce, contract_hash,
                                                         deployed);
        if (!r.has_value()) {
          return NativeContractBuilder::host_fn_error_conversion(r.error());
        }
        if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
          // The sdk's deploy_contract syscall_throw()s here: a frame fault,
          // not a WalletError.
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return deploy_result_t::make_ok(deployed);
      });

  assert(builder.check_methods<default_wallet::DefaultWallet>());
  return builder.build();
}

} // namespace hogd
