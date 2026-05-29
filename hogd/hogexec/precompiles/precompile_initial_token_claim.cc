#include "hogd/hogexec/precompiles/precompile_initial_token_claim.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "built_contracts/initial_token_claim.h"
#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/contract_result.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
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

  // Domain separator
  std::string_view prefix = "Initial Deployment";

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

InvokeStatus<uint128_t>
load_allocation(NativeSyscallContext &ctx,
                StorageKeyContractData const &contract_data_key)
{
  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r =
      ctx.syscall<SyscallNo::kNonnegativeIntGet>(contract_data_key, true,
                                                 output_set, output_set_opt,
                                                 output_add);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  if (!output_set_opt) {
    panic("failed to get set value from storage key, after calling nnint_get "
          "with load_from_snapshot=true");
  }
  auto [total, of] = overflowing_add(output_set, output_add);
  if (of || total < 0) {
    trace("overflowing_add failed in load_allocation; we must have configured "
          "genesis incorrectly");
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  return total;
}

InvokeStatus<void>
delete_storage_key(NativeSyscallContext &ctx,
                   StorageKeyContractData const &contract_data_key)
{
  auto r = ctx.syscall<SyscallNo::kDeleteObject>(
      contract_data_key, std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  return {};
}

} // namespace

std::unique_ptr<NativeContract>
make_native_initial_token_claim_contract()
{
  NativeContractBuilder builder;

  builder.add<initial_token_claim::InitialTokenClaim, "claim_initial_tokens">(
      [](NativeSyscallContext &ctx, SHA256Hash hash, SHA256Hash nonce,
         ContractEntrypointID entrypoint,
         x23::BufView calldata) -> InvokeStatus<void> {
        trace("initial token claim claim_initial_tokens");

        // Create storage key: sha256("Initial Deployment" || hash || nonce ||
        // entrypoint || calldata)
        StorageKeyContractData const &contract_data_key =
            calculate_storage_key(hash, nonce, entrypoint, calldata);

        auto allocation = load_allocation(ctx, contract_data_key);
        if (!allocation.has_value()) {
          return allocation.error();
        }

        // Delete the storage key, to prevent double claiming
        if (auto err = delete_storage_key(ctx, contract_data_key);
            !err.has_value()) {
          return err;
        }

        // Calculate receiver address from contract hash and nonce
        auto receiver_addr = SystemCallHandler::static_get_deploy_address(
            ctx.handler().get_current_address(), nonce, hash);

        // Transfer tokens to the receiver via HOG token contract
        auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        std::tuple<uint64_t, uint64_t> allocation_tuple =
            std::make_tuple(*allocation >> 64,
                            *allocation & 0xffffffffffffffffULL);

        auto transfer_result =
            native_token_client.invoke_opt<"transfer">(ctx, receiver_addr,
                                                       allocation_tuple);

        if (!transfer_result.has_value() ||
            !transfer_result.value().has_value() ||
            !transfer_result.value().value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto transfer_ok = transfer_result.value().value().value();
        if (!transfer_ok.is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Deploy the contract using hash and nonce
        ContractAddress deployed_addr{};
        auto deploy_res =
            ctx.syscall<SyscallNo::kDeployContract>(nonce, hash, deployed_addr);
        if (!deploy_res.has_value()) {
          return NativeContractBuilder::host_fn_error_conversion(
              deploy_res.error());
        }
        if (*deploy_res != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call the initialize entrypoint with provided calldata
        CopyableInvokeInputs inputs{
            .entrypoint_id = sdk_method_id(entrypoint),
            .gas_limit = static_cast<GasAmount>(UINT64_MAX),
            .flags = std::to_underlying(InvokeFlag::None),
        };
        CopyableInvokeOutputs outputs{};

        auto invoke_result = ctx.syscall<SyscallNo::kInvoke>(
            deployed_addr, std::as_bytes(std::span(calldata)),
            /*auth tokens*/ std::span<uint32_t>{}, inputs, outputs);

        if (!invoke_result.has_value()) {
          return NativeContractBuilder::host_fn_error_conversion(
              invoke_result.error());
        }
        if (*invoke_result != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  assert(builder.check_methods<initial_token_claim::InitialTokenClaim>());
  return builder.build();
}
} // namespace hogd
