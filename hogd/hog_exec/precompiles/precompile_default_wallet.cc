#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/ffi_helpers.h"
#include "hogd/hog_exec/components/ffi_types.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

namespace {

using default_wallet::WalletCall;
using default_wallet::WalletError;

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

// Reference constants (implementation.rs).
constexpr size_t kMaxCalls = 16;
constexpr size_t kMaxReturnDataBytes = kMaxReturnBufferLen; // 1024

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
      // The builder decays this lambda to a function pointer and deduces the
      // deserialized argument tuple from its parameter types, so `calls` must
      // be taken by value (a reference would not satisfy serializes_like).
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      [](NativeSyscallContext &ctx, x23::Vec<WalletCall, 4294967295> calls)
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

        // Running CXS size of the eventual Vec<WalletBytes> return: the
        // outer length prefix now, each item's prefix + bytes as it lands.
        // All operands are bounded (<= 16 calls, <= 1024-byte returns), so
        // the reference's u32/checked_add overflow guards are unreachable
        // here and elided.
        wallet_bytes_vec_t returns;
        size_t encoded_return_data_len =
            x23::cxs_size(static_cast<uint32_t>(calls.size()));

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
            return NativeContractBuilder::host_fn_error_conversion(inv.error());
          }
          if (*inv != SyscallErrorCode::kSyscallErrorCodeNone) {
            // Rejected before a child frame ran: sdk contract_call Err(_).
            return execute_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }
          if (outputs.frame_status != InvocationFrameStatusCode::Return &&
              outputs.frame_status != InvocationFrameStatusCode::Abort) {
            // Fault or unexpected status: sdk contract_call Err(Fault).
            return execute_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }

          // Fetch the child's payload — return or abort bytes — as the
          // sdk's fetch_frame_payload does for both outcomes. The wallet
          // forwards no auth tokens and drops any returned ids, so pass an
          // empty auth span (the host accepts zero-length spans).
          std::vector<uint8_t> return_buf(outputs.return_buffer_len);
          RuntimeMemoryLength return_written = 0;
          RuntimeMemoryLength auth_written = 0;
          auto get = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
              std::as_writable_bytes(std::span(return_buf)), return_written,
              std::span<std::byte>{}, auth_written);
          if (!get.has_value()) {
            return NativeContractBuilder::host_fn_error_conversion(get.error());
          }
          // Unreachable with a correct host (after a Return/Abort frame the
          // payload exists and the syscall succeeds), but the sdk maps a
          // fetch failure — error code or short write (checked_returned_len)
          // — to CallFaulted; mirror it exactly.
          if (*get != SyscallErrorCode::kSyscallErrorCodeNone ||
              return_written != return_buf.size()) {
            return execute_result_t::make_err(
                WalletException{WalletError::CallFaulted});
          }

          if (outputs.frame_status == InvocationFrameStatusCode::Abort) {
            // sdk contract_call: payload fetched (and here dropped), then
            // the abort is reported.
            return execute_result_t::make_err(
                WalletException{WalletError::CallAborted});
          }

          encoded_return_data_len +=
              x23::cxs_size(static_cast<uint32_t>(return_buf.size())) +
              return_buf.size();
          if (encoded_return_data_len > kMaxReturnDataBytes) {
            return execute_result_t::make_err(
                WalletException{WalletError::ReturnDataTooLarge});
          }

          wallet_bytes_t item;
          item.append_range(return_buf);
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
          // The sdk's deploy_contract syscall_throw()s on error — a library
          // abort encoding LibraryExn::Syscall{kDeployContract, errno}, not a
          // WalletError and not a frame fault.
          LibraryExn exn(LibraryExnKind::kLibraryExnSyscall);
          exn.syscall_exn() = SyscallExn{
              .call_number = SyscallNo::kDeployContract,
              .err_number = static_cast<SysErr>(*r),
          };
          return deploy_result_t::make_err(LibraryException{std::move(exn)});
        }
        return deploy_result_t::make_ok(deployed);
      });

  assert(builder.check_methods<default_wallet::DefaultWallet>());
  return builder.build();
}

} // namespace hogd
