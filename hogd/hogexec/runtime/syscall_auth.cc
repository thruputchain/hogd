
#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/hogexec/runtime/ffi_helpers.h"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <new>
#include <span>
#include <utility>

#include "address_auth.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/rust/contract_types/bindings.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
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
    return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, 0};
  }

  if (!new_key) {
    return {new_key.error().code(), 0};
  }

  auto status = syscall_handler.change_public_key_registration(input_key_index,
                                                               new_key.value());
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<
              SyscallNo::kChangePublicKeyRegistration>()) {
    try {
      trace_data->key_index = input_key_index;
      if (optional_input_key_scheme) {
        trace_data->key_scheme = *optional_input_key_scheme;
      }
      auto const *kb_data =
          reinterpret_cast<const uint8_t *>(input_key_bytes.data());
      trace_data->key_bytes.append(kb_data,
                                             kb_data + input_key_bytes.size());
      if (optional_input_key_weight) {
        trace_data->key_weight = *optional_input_key_weight;
      }
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kChangePublicKeyRegistration, e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_WriteAddressNonceConfig_inner(
    SystemCallHandler &syscall_handler,
    const generated::NonceConfig &input_nonce_config) noexcept
{
  auto nonce_config =
      FFIHelperType<generated::NonceConfig>::to_cpp(input_nonce_config);
  auto status = syscall_handler.set_address_nonce_config(nonce_config);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<
              SyscallNo::kWriteAddressNonceConfig>()) {
    try {
      trace_data->nonce_config = nonce_config;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kWriteAddressNonceConfig, e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_WriteAddressGasPermissionConfigs_inner(
    SystemCallHandler &syscall_handler,
    const generated::GasPermissionConfig *optional_input_permission_0,
    const generated::GasPermissionConfig *optional_input_permission_1,
    const generated::GasPermissionConfig *optional_input_permission_2,
    const generated::GasPermissionConfig *optional_input_permission_3,
    const generated::GasPermissionConfig *optional_input_permission_4) noexcept
{
  x23::Vec<GasPermissionConfig, kMaxGasConfigs> gas_permission_configs;
  bool no_gaps = false;
  bool found_gap = false;

  try {
    auto add_gas_permission =
        [&](const generated::GasPermissionConfig *permission) {
          if (permission) {
            if (no_gaps) {
              found_gap = true;
              return;
            }
            gas_permission_configs.push_back(
                FFIHelperType<generated::GasPermissionConfig>::to_cpp(
                    *permission));
          }
          else {
            no_gaps = true;
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
    return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, 0};
  }

  if (found_gap) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }

  auto status = syscall_handler.set_address_gas_permission_configs(
      std::move(gas_permission_configs));
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<
              SyscallNo::kWriteAddressGasPermissionConfigs>()) {
    try {
      // Reconstruct the permissions from the original inputs
      auto add_perm =
          [&](const generated::GasPermissionConfig *perm) {
            if (perm) {
              trace_data->permissions.push_back(
                  FFIHelperType<generated::GasPermissionConfig>::to_cpp(
                      *perm));
            }
          };
      add_perm(optional_input_permission_0);
      add_perm(optional_input_permission_1);
      add_perm(optional_input_permission_2);
      add_perm(optional_input_permission_3);
      add_perm(optional_input_permission_4);
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kWriteAddressGasPermissionConfigs, e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthClaimCount_inner(SystemCallHandler &syscall_handler,
                                uint32_t &output_count) noexcept
{
  output_count = syscall_handler.get_auth_claims().size();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetAuthClaimCount>()) {
    try {
      trace_data->output.success().count = output_count;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetAuthClaimCount,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthClaim_inner(SystemCallHandler &syscall_handler,
                           uint32_t input_index,
                           generated::AuthClaim &output_auth_claim) noexcept
{
  if (input_index >= syscall_handler.get_auth_claims().size()) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }
  auto const &claim = syscall_handler.get_auth_claims()[input_index];
  output_auth_claim = FFIHelperType<generated::AuthClaim>::to_ffi(claim);

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetAuthClaim>()) {
    try {
      trace_data->index = input_index;
      trace_data->output.success().auth_claim = claim;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetAuthClaim,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
