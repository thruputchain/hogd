#include "hogd/hogexec/runtime/syscall_inner.h"

#include <concepts>
#include <cstddef>
#include <exception>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

// Computes the SHA256 hash of the input buffer and returns the gas cost
// Returns: Status code and gas cost for the operation
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(SystemCallHandler &handler,
                     std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash) noexcept
{
  output_hash = sha256(input_buffer);

  if (auto *stats = handler.get_transaction_stats()) {
    stats->sha256_total_bytes_processed += input_buffer.size();
  }
  if (auto *trace_data = handler.get_syscall_trace_data<SyscallNo::kSHA256>()) {
    try {
      auto const *data = reinterpret_cast<const uint8_t *>(input_buffer.data());
      trace_data->input.append(data, data + input_buffer.size());
      trace_data->output.success().hash = output_hash;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kSHA256, e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          SHA256HashCost(input_buffer.size())};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256Update_inner(SystemCallHandler &handler, uint32_t input_index,
                           std::span<const std::byte> input_buffer,
                           bool input_reset) noexcept
{
  auto r = handler.sha256_update(input_index, input_buffer, input_reset);
  if (!r.has_value()) {
    return {r.error().code(), 0};
  }
  if (auto *trace_data =
          handler.get_syscall_trace_data<SyscallNo::kSHA256Update>()) {
    try {
      trace_data->index = input_index;
      auto const *data =
          reinterpret_cast<const uint8_t *>(input_buffer.data());
      trace_data->buffer.append(data, data + input_buffer.size());
      trace_data->reset = input_reset;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kSHA256Update,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256Finalize_inner(SystemCallHandler &handler, uint32_t input_index,
                             SHA256Hash &output_output) noexcept
{
  auto r = handler.sha256_finalize(input_index);
  if (!r.has_value()) {
    return {r.error().code(), 0};
  }
  output_output = r.value();
  if (auto *trace_data =
          handler.get_syscall_trace_data<SyscallNo::kSHA256Finalize>()) {
    try {
      trace_data->index = input_index;
      trace_data->output.success().hash = output_output;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kSHA256Finalize,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

// Verifies a cryptographic signature against a message using the provided
// public key
// Returns: Status code indicating if verification succeeded and gas cost
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_VerifySignature_inner(
  SystemCallHandler &handler,
  int32_t scheme,
                              std::span<const std::byte> signature,
                              std::span<const std::byte> public_key,
                              std::span<const std::byte> message) noexcept
{
  static_assert(std::same_as<decltype(scheme),
                             std::underlying_type_t<CryptographicScheme>>);
  
  switch (static_cast<CryptographicScheme>(scheme)) {
  case CryptographicScheme::Ed25519:
    if (public_key.size() != sizeof(Ed25519PublicKey) ||
        signature.size() != sizeof(Ed25519Signature)) {
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
    }
    if (!ed25519_verify(public_key.subspan<0, sizeof(Ed25519PublicKey)>(),
                        signature.subspan<0, sizeof(Ed25519Signature)>(),
                        message)) {
      trace("Signature verification failed");
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
    }
  default:
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }

  if (auto *trace_data =
        handler.get_syscall_trace_data<SyscallNo::kVerifySignature>()) {
    try {
      trace_data->scheme = scheme;
      trace_data->signature.append(reinterpret_cast<const uint8_t *>(signature.data()), reinterpret_cast<const uint8_t *>(signature.data() + signature.size()));
      trace_data->public_key.append(reinterpret_cast<const uint8_t *>(public_key.data()), reinterpret_cast<const uint8_t *>(public_key.data() + public_key.size()));
      trace_data->message.append(reinterpret_cast<const uint8_t *>(message.data()), reinterpret_cast<const uint8_t *>(message.data() + message.size()));
      } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kVerifySignature,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
