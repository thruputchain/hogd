#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

// Computes the SHA256 hash of the input buffer and returns the gas cost
// Returns: Status code and gas cost for the operation
AsyncSyscallInnerResult
syscall_SHA256_inner(SystemCallHandler &handler,
                     std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  output_hash = sha256(input_buffer);

  if (auto *stats = handler.get_transaction_stats()) {
    stats->sha256_total_bytes_processed += input_buffer.size();
  }
  if (auto *trace_data = handler.get_syscall_trace_data<SyscallNo::kSHA256>()) {
    trace_data->outputs.emplace(SyscallSHA256Outputs{.hash = output_hash});
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{
                                   SHA256HashCost(input_buffer.size())}};
};

AsyncSyscallInnerResult
syscall_SHA256Update_inner(SystemCallHandler &handler, uint32_t input_index,
                           std::span<const std::byte> input_buffer,
                           bool input_reset)
{
  auto r = handler.sha256_update(input_index, input_buffer, input_reset);
  if (!r.has_value()) {
    co_return SyscallInnerResult{r.error().code(), WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

AsyncSyscallInnerResult
syscall_SHA256Finalize_inner(SystemCallHandler &handler, uint32_t input_index,
                             SHA256Hash &output_output)
{
  auto r = handler.sha256_finalize(input_index);
  if (!r.has_value()) {
    co_return SyscallInnerResult{r.error().code(), WrappedGasAmount{0}};
  }
  output_output = *r;
  if (auto *trace_data =
          handler.get_syscall_trace_data<SyscallNo::kSHA256Finalize>()) {
    trace_data->outputs.emplace(
        SyscallSHA256FinalizeOutputs{.hash = output_output});
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

// Verifies a cryptographic signature against a message using the provided
// public key
// Returns: Status code indicating if verification succeeded and gas cost
AsyncSyscallInnerResult
syscall_VerifySignature_inner(int32_t input_scheme,
                              std::span<const std::byte> input_signature,
                              std::span<const std::byte> input_public_key,
                              std::span<const std::byte> input_message)
{
  static_assert(std::same_as<decltype(input_scheme),
                             std::underlying_type_t<CryptographicScheme>>);

  switch (static_cast<CryptographicScheme>(input_scheme)) {
  case CryptographicScheme::Ed25519:
    if (input_public_key.size() != sizeof(Ed25519PublicKey) ||
        input_signature.size() != sizeof(Ed25519Signature)) {
      co_return SyscallInnerResult{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
          WrappedGasAmount{0}};
    }
    if (!ed25519_verify(input_public_key.subspan<0, sizeof(Ed25519PublicKey)>(),
                        input_signature.subspan<0, sizeof(Ed25519Signature)>(),
                        input_message)) {
      trace("Signature verification failed");
      co_return SyscallInnerResult{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
          WrappedGasAmount{0}};
    }
    co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                                 WrappedGasAmount{0}};
  default:
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }
}

} // namespace hogd
