#include "hogd/hogexec/precompiles/precompile_initial_token_claim.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/initial_token_claim.h"
#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

StorageKeyContractData
calculate_storage_key(SHA256Hash const &hash, SHA256Hash const &nonce,
                      ContractEntrypointID entrypoint,
                      std::span<const std::uint8_t> calldata)
{
  // Create the key data: "Initial Deployment" || hash || nonce || entrypoint ||
  // calldata
  std::vector<std::uint8_t> key_data;

  // Add prefix
  std::string_view prefix = "Initial Deployment";
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
load_allocation(SystemCallHandler &handler,
                StorageKeyContractData const &contract_data_key)
{
  auto nnint_result = handler.get_nnint(contract_data_key, true);
  if (!nnint_result.has_value()) {
    return nnint_result.error().to_invoke_error();
  }
  if (!nnint_result.value().has_value()) {
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  }
  auto [set, add] = nnint_result.value().value();
  if (!set.has_value()) {
    panic("failed to get set value from storage key, after calling nnint_get "
          "with load_from_snapshot=true");
  }
  auto [total, of] = overflowing_add(*set, add);
  if (of || total < 0) {
    trace("overflowing_add failed in load_allocation; we must have configured "
          "genesis incorrectly");
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  }
  return total;
}

InvokeStatus<void>
delete_storage_key(SystemCallHandler &handler,
                   StorageKeyContractData const &contract_data_key)
{
  auto delete_result = handler.delete_object(
      contract_data_key,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagExclusiveLock));
  if (!delete_result.has_value()) {
    return delete_result.error().to_invoke_error();
  }

  if (!delete_result.value().has_value()) {
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  }
  return {};
}

} // namespace

std::unique_ptr<NativeContract>
make_native_initial_token_claim_contract()
{
  NativeContractBuilder builder;

  builder.add<initial_token_claim::InitialTokenClaim, "claim_initial_tokens">(
      [](SystemCallHandler &handler, NativeRuntime & /*unused*/,
         SHA256Hash hash, SHA256Hash nonce, ContractEntrypointID entrypoint,
         x23::Vec<uint8_t, 4294967295> calldata) -> InvokeStatus<void> {
        trace("initial token claim claim_initial_tokens");

        // Create storage key: sha256("Initial Deployment" || hash || nonce ||
        // entrypoint || calldata)
        auto contract_data_key =
            calculate_storage_key(hash, nonce, entrypoint, calldata);

        auto allocation = load_allocation(handler, contract_data_key);
        if (!allocation.has_value()) {
          return allocation.error();
        }

        // Delete the storage key, to prevent double claiming
        if (auto err = delete_storage_key(handler, contract_data_key);
            !err.has_value()) {
          return err;
        }

        // Calculate receiver address from contract hash and nonce
        auto receiver_addr = SystemCallHandler::static_get_deploy_address(
            handler.get_current_address(), nonce, hash);

        // Transfer tokens to the receiver via HOG token contract
        auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        auto [transfer_result, gas_used] =
            native_token_client.invoke_opt<"transfer">(handler, receiver_addr,
                                                       to_xdr(*allocation));

        if (!transfer_result.has_value() ||
            !transfer_result.value().has_value() ||
            !transfer_result.value().value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        auto transfer_ok = transfer_result.value().value().value();
        if (!transfer_ok.is_ok()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // Deploy the contract using hash and nonce
        auto deploy_result = handler.deploy_contract(nonce, hash);
        if (!deploy_result.has_value()) {
          return deploy_result.error().to_invoke_error();
        }
        if (!deploy_result.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        auto deployed_addr = deploy_result.value().value();

        // Call the initialize entrypoint with provided calldata
        auto method_id = sdk_method_id(entrypoint);

        auto [invoke_result, invoke_gas] =
            handler.invoke(deployed_addr, method_id, UINT64_MAX, calldata,
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));

        if (!invoke_result.has_value()) {
          switch (invoke_result.error().code()) {
          case HostFnErrorCode::kHostOutOfGas:
            return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
          case HostFnErrorCode::kHostReturn:
          case HostFnErrorCode::kHostExit:
            panic("invoke() cannot return these error codes");
          case HostFnErrorCode::kHostNoneOrRecoverable:
            panic("HostFnErrorCode::kHostNoneOrRecoverable is not expected if "
                  "!invoke_result.has_value()");
          case HostFnErrorCode::kHostUnrecoverable:
            return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
          case HostFnErrorCode::kHostValidationFailure:
            return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
          case HostFnErrorCode::kHostAbortTransaction:
            return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
            break;
          }
        }
        if (invoke_result.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  assert(builder.check_methods<initial_token_claim::InitialTokenClaim>());
  return builder.build();
}
} // namespace hogd
