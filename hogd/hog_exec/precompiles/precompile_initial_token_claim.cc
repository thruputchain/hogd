#include "hogd/hog_exec/precompiles/precompile_initial_token_claim.h"
#include "invoke_args.hh"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/initial_token_claim.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

StorageKeyContractData
calculate_storage_key(SHA256Hash const &hash, SHA256Hash const &nonce,
                      ContractEntrypointID entrypoint, x23::BufView calldata)
{
  // Create the key data: "Initial Deployment" || hash || nonce || entrypoint ||
  // calldata
  std::vector<std::uint8_t> key_data;

  // Domain separator. Use a `u8"..."` literal so the hashed bytes are the
  // UTF-8 (== ASCII, here) encoding mandated by the standard rather than
  // implementation-defined ordinary-literal code units; otherwise the derived
  // storage key would depend on the execution character set (`-fexec-charset`).
  // See hogd issue #2573.
  std::u8string_view prefix = u8"Initial Deployment";

  // reserve space for the key data
  key_data.reserve(prefix.size() + hash.size() + nonce.size() +
                   entrypoint.size() + calldata.size());

  // Add prefix
  key_data.insert(key_data.end(), prefix.begin(), prefix.end());

  // Add hash
  key_data.insert(key_data.end(), hash.begin(), hash.end());

  // Add nonce
  key_data.insert(key_data.end(), nonce.begin(), nonce.end());

  // Add entrypoint (as big-endian bytes)
  key_data.insert(key_data.end(), entrypoint.begin(), entrypoint.end());

  // Add calldata
  key_data.insert(key_data.end(), calldata.begin(), calldata.end());

  return sha256(key_data);
}

x23::Async<InvokeStatus<uint128_t>>
load_allocation(NativeSyscallContext &ctx,
                StorageKeyContractData const &contract_data_key)
{
  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
      contract_data_key, true, output_set, output_set_opt, output_add);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }

  if (!output_set_opt) {
    panic("failed to get set value from storage key, after calling nnint_get "
          "with load_from_snapshot=true");
  }
  auto [total, of] = overflowing_add(output_set, output_add);
  if (of || total < 0) {
    trace("overflowing_add failed in load_allocation; we must have configured "
          "genesis incorrectly");
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return total;
}

x23::Async<InvokeStatus<void>>
delete_storage_key(NativeSyscallContext &ctx,
                   StorageKeyContractData const &contract_data_key)
{
  auto r = co_await ctx.syscall<SyscallNo::kDeleteObject>(
      contract_data_key, std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return InvokeStatus<void>{};
}

} // namespace

std::unique_ptr<NativeContract>
make_native_initial_token_claim_contract()
{
  NativeContractBuilder builder;

  builder.add<initial_token_claim::InitialTokenClaim, u8"claim_initial_tokens">(
      [](NativeSyscallContext &ctx, SHA256Hash hash, SHA256Hash nonce,
         ContractEntrypointID entrypoint,
         x23::BufView calldata) -> x23::Async<InvokeStatus<void>> {
        trace("initial token claim claim_initial_tokens");

        // Create storage key: sha256("Initial Deployment" || hash || nonce ||
        // entrypoint || calldata)
        StorageKeyContractData const &contract_data_key =
            calculate_storage_key(hash, nonce, entrypoint, calldata);

        auto allocation = co_await load_allocation(ctx, contract_data_key);
        if (!allocation.has_value()) {
          co_return allocation.error();
        }

        // Delete the storage key, to prevent double claiming
        if (auto err = co_await delete_storage_key(ctx, contract_data_key);
            !err.has_value()) {
          co_return err;
        }

        // Calculate receiver address from contract hash and nonce
        auto receiver_addr = SystemCallHandler::static_get_deploy_address(
            ctx.handler().get_current_address(), nonce, hash);

        // Genesis claims are initial token allocations, not ERC20 transfers:
        // move the balance directly (no child frame, no Transfer event)
        // instead of invoking the strict public `transfer`, which cannot see
        // the commit-time storage refund of the exclusive delete above. HogDb
        // validates the claim's net balance at commit, where the
        // exclusive-delete refund nets against this debit exactly.
        auto self_addr = ctx.handler().get_current_address();
        if (auto moved = co_await native_token_unchecked_transfer(
                ctx.handler(), self_addr, receiver_addr,
                static_cast<int128_t>(*allocation));
            !moved.has_value()) {
          co_return moved.error();
        }

        // Deploy the contract using hash and nonce
        ContractAddress deployed_addr{};
        auto deploy_res =
            co_await ctx.syscall<SyscallNo::kDeployContract>(nonce, hash,
                                                             deployed_addr);
        if (!deploy_res.has_value()) {
          co_return NativeContractBuilder::host_fn_error_conversion(
              deploy_res.error());
        }
        if (*deploy_res != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call the initialize entrypoint with provided calldata
        CopyableInvokeInputs inputs{
            .entrypoint_id = sdk_method_id(entrypoint),
            .gas_limit = static_cast<GasAmount>(UINT64_MAX),
            .flags = std::to_underlying(InvokeFlag::None),
        };
        CopyableInvokeOutputs outputs{};

        auto invoke_result = co_await ctx.syscall<SyscallNo::kInvoke>(
            deployed_addr, std::as_bytes(std::span(calldata)),
            /*auth tokens*/ std::span<uint32_t>{}, inputs, outputs);

        if (!invoke_result.has_value()) {
          co_return NativeContractBuilder::host_fn_error_conversion(
              invoke_result.error());
        }
        // Propagate a failed initializer: a nonzero code means it was rejected
        // before running, and a child frame that ran but did not return
        // normally (Abort/Fault) must revert the whole claim too.
        if (*invoke_result != SyscallErrorCode::kSyscallErrorCodeNone ||
            outputs.frame_status != InvocationFrameStatusCode::Return) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  assert(builder.check_methods<initial_token_claim::InitialTokenClaim>());
  return builder.build();
}
} // namespace hogd
