

#include "auth_token.hh"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <new>
#include <span>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/ffi_helpers.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "hogd/rust/contract_types/bindings.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {
constexpr bool
valid_accepted_keys_bitmap(BitMap bitmap)
{
  static_assert(kMaxSigners <= 32);
  static_assert(sizeof(BitMap) == sizeof(uint32_t));
  return (bitmap & (uint64_t{0xFFFF'FFFF} << kMaxSigners)) == 0;
}
} // namespace

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_ChangePublicKeyRegistration_inner(
    SystemCallHandler &syscall_handler, uint32_t input_key_index,
    const int32_t *optional_input_key_scheme,
    std::span<const std::byte> input_key_bytes,
    const uint32_t *optional_input_key_weight) noexcept
{
  SyscallStatus<PublicKeyConfigOpt> new_key;
  try {
    new_key = [&] -> SyscallStatus<PublicKeyConfigOpt> {
      // Either all of the args are empty (deleting the key) or they're all
      // present and valid
      if (optional_input_key_scheme == nullptr) {
        if (optional_input_key_weight != nullptr) {
          return SyscallError{
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
        }
        if (!input_key_bytes.empty()) {
          return SyscallError{
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
        }
        return {nullptr};
      }
      // the key must exist and be valid
      if (optional_input_key_weight == nullptr) {
        return SyscallError{
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
      }

      PublicKey key;

      switch (static_cast<CryptographicScheme>(*optional_input_key_scheme)) {
      case CryptographicScheme::Ed25519:
      {
        key.scheme(CryptographicScheme::Ed25519);
        if (input_key_bytes.size() != key.ed25519().size()) {
          return SyscallError{
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
        }
        std::memcpy(key.ed25519().data(), input_key_bytes.data(),
                    sizeof(Ed25519PublicKey));

        return {x23::Ptr<PublicKeyConfig>{
            new PublicKeyConfig{.key = key,
                                .max_weight = *optional_input_key_weight}}};
      }
      default:
        trace("Received unknown cryptographic scheme in "
              "ChangePublicKeyRegistration: {}",
              *optional_input_key_scheme);
        return SyscallError{
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
      }
    }();
  } catch (std::bad_alloc &e) {
    error("unrecoverable: failed to allocate public key config: {}", e.what());
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }

  if (!new_key) {
    return {new_key.error().code(), WrappedGasAmount{0}};
  }

  auto status = syscall_handler.change_public_key_registration(input_key_index,
                                                               new_key.value());
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!status.value()) {
    return {status.value().error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_WriteAddressNonceConfig_inner(
    SystemCallHandler &syscall_handler,
    const generated::NonceConfig &input_nonce_config) noexcept
{
  if (!valid_accepted_keys_bitmap(input_nonce_config.accepted_keys)) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }

  auto nonce_config =
      FFIHelperType<generated::NonceConfig>::to_cpp(input_nonce_config);
  auto status = syscall_handler.set_address_nonce_config(nonce_config);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!status.value()) {
    return {status.value().error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_WriteAddressGasPermissionConfigs_inner(
    SystemCallHandler &syscall_handler,
    const generated::GasPermissionConfig *optional_input_permission_0,
    const generated::GasPermissionConfig *optional_input_permission_1,
    const generated::GasPermissionConfig *optional_input_permission_2,
    const generated::GasPermissionConfig *optional_input_permission_3,
    const generated::GasPermissionConfig *optional_input_permission_4) noexcept
{
  x23::Vec<GasPermissionConfig, kMaxGasConfigs> gas_permission_configs;
  bool found_first_empty = false;
  bool found_gap = false;
  bool invalid_bitmap = false;

  try {
    auto add_gas_permission =
        [&](const generated::GasPermissionConfig *permission) {
          if (permission) {
            if (found_first_empty) {
              found_gap = true;
              return;
            }
            if (!valid_accepted_keys_bitmap(permission->accepted_keys)) {
              invalid_bitmap = true;
              return;
            }
            gas_permission_configs.push_back(
                FFIHelperType<generated::GasPermissionConfig>::to_cpp(
                    *permission));
          }
          else {
            found_first_empty = true;
          }
        };

    add_gas_permission(optional_input_permission_0);
    add_gas_permission(optional_input_permission_1);
    add_gas_permission(optional_input_permission_2);
    add_gas_permission(optional_input_permission_3);
    add_gas_permission(optional_input_permission_4);
  } catch (std::exception &e) {
    error("unrecoverable: failed to add bounded number of gas permission "
          "configs to vector of bounded size: {}",
          e.what());
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }

  if (found_gap || invalid_bitmap) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }

  auto status = syscall_handler.set_address_gas_permission_configs(
      std::move(gas_permission_configs));
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!status.value()) {
    return {status.value().error().code(), WrappedGasAmount{0}};
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetAuthClaimCount_inner(SystemCallHandler &syscall_handler,
                                uint32_t &output_count) noexcept
{
  output_count = syscall_handler.get_auth_claims().size();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetAuthClaimCount>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetAuthClaimCountOutputs{.count = output_count});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetAuthClaimCount,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetAuthClaim_inner(SystemCallHandler &syscall_handler,
                           uint32_t input_index,
                           generated::AuthClaim &output_auth_claim) noexcept
{
  if (input_index >= syscall_handler.get_auth_claims().size()) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  auto const &claim = syscall_handler.get_auth_claims()[input_index];
  output_auth_claim = FFIHelperType<generated::AuthClaim>::to_ffi(claim);

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetAuthClaim>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetAuthClaimOutputs{.auth_claim = claim});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetAuthClaim,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_MintAuthToken_inner(
    SystemCallHandler &syscall_handler, BlockHeight input_expiry_height,
    std::span<const std::byte> input_serialized_auth_token,
    uint32_t &output_token_id) noexcept
{
  AuthToken auth_token{.mint_address = syscall_handler.get_current_address(),
                       .expiry_height = input_expiry_height};
  try {
    x23::deserialize(&auth_token.root_node,
                     x23::BufView{input_serialized_auth_token});
  } catch (x23::XdrException &e) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  } catch (...) {
    error("unrecoverable: failed to deserialize auth token with unknown "
          "exception");
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }

  auto status = syscall_handler.mint_auth_token(auth_token);
  if (!status) {
    return {status.error().code(), WrappedGasAmount{0}};
  }
  output_token_id = *status;

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_ConsumeAuthToken_inner(
    SystemCallHandler &syscall_handler, uint32_t input_token_id,
    std::span<std::byte> output_auth_payload,
    uint32_t &output_unwrapped_token_id_start,
    uint32_t &output_unwrapped_token_id_count) noexcept
{
  auto height = syscall_handler.get_block_height();
  if (!height) {
    return {height.error().to_host_fn_error(), WrappedGasAmount{0}};
  }

  auto status = syscall_handler.consume_auth_token(input_token_id, *height);
  if (!status) {
    return {status.error().code(), WrappedGasAmount{0}};
  }

  auto const &result = *status;

  auto payload_bytes = std::as_bytes(
      std::span<const uint8_t>{result.payload.data(), result.payload.size()});
  auto copy_len =
      std::min<size_t>(payload_bytes.size(), output_auth_payload.size());
  if (copy_len > 0) {
    std::memcpy(output_auth_payload.data(), payload_bytes.data(), copy_len);
  }

  output_unwrapped_token_id_start = result.new_token_id_start;
  output_unwrapped_token_id_count = result.new_token_id_count;

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kConsumeAuthToken>()) {
    try {
      trace_data->outputs.emplace(SyscallConsumeAuthTokenOutputs{
          .payload = result.payload,
          .unwrapped_token_id_start = result.new_token_id_start,
          .unwrapped_token_id_count = result.new_token_id_count});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kConsumeAuthToken,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
