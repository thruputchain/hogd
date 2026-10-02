

#include "auth_token.hh"
#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/ffi_helpers.h"
#include "hogd/hog_exec/components/ffi_types.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_buffer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

MaybeAsyncSyscallInnerResult
syscall_ChangePublicKeyRegistration_inner(
    SystemCallHandler &syscall_handler, uint32_t input_key_index,
    const int32_t *optional_input_key_scheme,
    std::span<const std::byte> input_key_bytes,
    const uint32_t *optional_input_key_weight)
{
  SyscallStatus<PublicKeyConfigOpt> new_key =
      [&] -> SyscallStatus<PublicKeyConfigOpt> {
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

  if (!new_key) {
    co_return SyscallInnerResult{new_key.error().code(), WrappedGasAmount{0}};
  }

  auto status = co_await syscall_handler.change_public_key_registration(
      input_key_index, std::move(*new_key));
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  auto &registration_status = *status;
  if (!registration_status) {
    co_return SyscallInnerResult{registration_status.error().code(),
                                 WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

MaybeAsyncSyscallInnerResult
syscall_WriteAddressRequiredWeight_inner(SystemCallHandler &syscall_handler,
                                         Weight input_required_weight)
{
  auto status = co_await syscall_handler.set_address_required_weight(
      input_required_weight);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  auto &weight_status = *status;
  if (!weight_status) {
    co_return SyscallInnerResult{weight_status.error().code(),
                                 WrappedGasAmount{0}};
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetAuthClaim_inner(SystemCallHandler &syscall_handler,
                           generated::AuthClaim &output_auth_claim)
{
  auto const &claim = syscall_handler.get_auth_claim();
  output_auth_claim = FFIHelperType<generated::AuthClaim>::to_ffi(claim);

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetAuthClaim>()) {
    trace_data->outputs.emplace(
        SyscallGetAuthClaimOutputs{.auth_claim = claim});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

namespace {

// AddressAuthData without the keys, as GetAddressAuthSummary reports it.
AddressAuthSummary
summarize(AddressAuthData const &auth_data)
{
  AddressAuthSummary summary{};
  for (size_t i = 0; i < kMaxSigners; ++i) {
    auto const &slot = auth_data.public_keys[i];
    if (!slot) {
      continue;
    }
    summary.public_keys[i] =
        PublicKeyConfigSummaryOpt{x23::Ptr<PublicKeyConfigSummary>{
            new PublicKeyConfigSummary{.scheme = slot->key.scheme(),
                                       .max_weight = slot->max_weight}}};
  }
  summary.required_weight = auth_data.required_weight;
  summary.last_written_height = auth_data.last_written_height;
  return summary;
}

// A registered key as the raw bytes of its scheme: the same bytes
// ChangePublicKeyRegistration took as key_bytes.
std::span<const std::byte>
public_key_bytes(PublicKey const &key)
{
  switch (key.scheme()) {
  case CryptographicScheme::Ed25519:
    return std::as_bytes(
        std::span<const uint8_t>{key.ed25519().data(), key.ed25519().size()});
  }
  panic("Unknown public key scheme in a registration: {}", key.scheme());
}

} // namespace

MaybeAsyncSyscallInnerResult
syscall_GetAddressAuthSummary_inner(SystemCallHandler &syscall_handler,
                                    std::span<std::byte> output_buffer,
                                    RuntimeMemoryLength &output_written_len)
{
  auto status = co_await syscall_handler.get_address_auth_data();
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  // Priced like NonnegativeIntGet: the access and any load are paid whatever
  // the outcome, since both happen before the read can answer.
  auto &[result, load_gas] = *status;
  const WrappedGasAmount gas{kStorageAccessCost + load_gas};
  if (!result.has_value()) {
    co_return SyscallInnerResult{result.error().code(), gas};
  }

  // The contract decodes the XDR itself; written_len reports the full
  // encoding so a short buffer can be retried.
  AddressAuthSummary summary = summarize(*result);
  auto serialized = x23::serialize(summary);
  output_written_len =
      copy_to_guest_buffer(output_buffer,
                           std::as_bytes(std::span(serialized.data(),
                                                   serialized.size())));

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetAddressAuthSummary>()) {
    trace_data->outputs.emplace(
        SyscallGetAddressAuthSummaryOutputs{.summary = std::move(summary)});
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

MaybeAsyncSyscallInnerResult
syscall_GetAddressPublicKey_inner(SystemCallHandler &syscall_handler,
                                  uint32_t input_key_index,
                                  std::span<std::byte> output_key,
                                  RuntimeMemoryLength &output_written_len)
{
  // Checked before the read, as ChangePublicKeyRegistration checks its index:
  // a slot number past the table reaches no storage and pays nothing beyond
  // the crossing.
  if (input_key_index >= kMaxSigners) {
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }

  auto status = co_await syscall_handler.get_address_auth_data();
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  auto &[result, load_gas] = *status;
  if (!result.has_value()) {
    co_return SyscallInnerResult{result.error().code(),
                                 WrappedGasAmount{kStorageAccessCost +
                                                  load_gas}};
  }

  auto const &slot = result->public_keys[input_key_index];
  if (!slot) {
    // An empty slot is a BytestringGet miss: the row had to be read to tell,
    // so the access and any load are paid.
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist,
        WrappedGasAmount{kStorageAccessCost + load_gas}};
  }

  auto key_bytes = public_key_bytes(slot->key);
  output_written_len = copy_to_guest_buffer(output_key, key_bytes);
  // Charged on the bytes that reach the guest, as BytestringGet charges, so
  // a scheme with large keys pays for its copy.
  const size_t copied = std::min(output_key.size(), key_bytes.size());
  const WrappedGasAmount gas{GasCost::bytestring_get(copied) + load_gas};

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetAddressPublicKey>()) {
    trace_data->outputs.emplace(
        SyscallGetAddressPublicKeyOutputs{.key = slot->key});
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_MintAuthToken_inner(
    SystemCallHandler &syscall_handler, const BlockHeight &input_expiry_height,
    std::span<const std::byte> input_serialized_auth_token,
    uint32_t &output_token_id)
{
  AuthToken auth_token{.mint_address = syscall_handler.get_current_address(),
                       .expiry_height = input_expiry_height};

  // Check expiry height is in the future
  if (input_expiry_height < syscall_handler.get_block_height()) {
    return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }

  // The wire type does not bound depth; decode under the chain's limit.
  try {
    x23::deserialize(&auth_token.root_node,
                     x23::BufView{input_serialized_auth_token},
                     size_t{
                         syscall_handler.config_params().max_auth_token_depth});
  } catch (x23::XdrException &e) {
    return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }

  auto status = syscall_handler.mint_auth_token(auth_token);
  if (!status) {
    return SyscallInnerResult{status.error().code(), WrappedGasAmount{0}};
  }
  output_token_id = *status;

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kMintAuthToken>()) {
    trace_data->outputs.emplace(
        SyscallMintAuthTokenOutputs{.token_id = output_token_id});
  }

  return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                            WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_ConsumeAuthToken_inner(
    SystemCallHandler &syscall_handler, uint32_t input_token_id,
    std::span<std::byte> output_auth_payload,
    ContractAddress &output_token_minter,
    RuntimeMemoryLength &output_auth_payload_written_len,
    uint32_t &output_unwrapped_token_id_start,
    uint32_t &output_unwrapped_token_id_count)
{
  auto status =
      syscall_handler.consume_auth_token(input_token_id,
                                         syscall_handler.get_block_height());
  if (!status) {
    return SyscallInnerResult{status.error().code(), WrappedGasAmount{0}};
  }

  auto const &result = *status;

  output_token_minter = *result.mint_address;

  auto payload_bytes = std::as_bytes(
      std::span<const uint8_t>{result.payload->data(), result.payload->size()});
  output_auth_payload_written_len =
      copy_to_guest_buffer(output_auth_payload, payload_bytes);

  output_unwrapped_token_id_start = result.new_token_id_start;
  output_unwrapped_token_id_count = result.new_token_id_count;

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kConsumeAuthToken>()) {
    trace_data->outputs.emplace(SyscallConsumeAuthTokenOutputs{
        .payload = *result.payload,
        .token_minter = *result.mint_address,
        .auth_payload_written_len = output_auth_payload_written_len,
        .unwrapped_token_id_start = result.new_token_id_start,
        .unwrapped_token_id_count = result.new_token_id_count});
  }

  return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                            WrappedGasAmount{0}};
}

} // namespace hogd
