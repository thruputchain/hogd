#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

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
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_SHA256_inner(SystemCallHandler &handler, WrappedGasAmount remaining_gas,
                     std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  const WrappedGasAmount required_gas{
      GasCost::sha256_hash(input_buffer.size())};
  if (remaining_gas.get() <= required_gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  output_hash = sha256(input_buffer);

  if (auto *stats = handler.get_transaction_stats()) {
    stats->sha256_total_bytes_processed += input_buffer.size();
  }
  if (auto *trace_data = handler.get_syscall_trace_data<SyscallNo::kSHA256>()) {
    trace_data->outputs.emplace(SyscallSHA256Outputs{.hash = output_hash});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, required_gas};
};

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_SHA256Update_inner(SystemCallHandler &handler,
                           WrappedGasAmount remaining_gas, uint32_t input_index,
                           std::span<const std::byte> input_buffer,
                           bool input_reset)
{
  const WrappedGasAmount required_gas{
      GasCost::sha256_update(input_buffer.size())};
  // Exact exhaustion also faults in the outer wrapper. Reject before hashing
  // or resetting a stream, and charge the remaining body budget.
  if (remaining_gas.get() <= required_gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }

  auto r = handler.sha256_update(input_index, input_buffer, input_reset);
  if (!r.has_value()) {
    return {r.error().code(), WrappedGasAmount{0}};
  }

  if (auto *stats = handler.get_transaction_stats()) {
    stats->sha256_total_bytes_processed += input_buffer.size();
  }

  // Whole blocks only, so this drops charges in two places: the carry from a
  // previous call, which can push one more compression than floor() counts,
  // and a tail of 56 bytes or more, whose padding spills a block that
  // Finalize's flat one does not cover. Both are one block, and the call that
  // caused them already paid the stream cost and the crossing -- more than the
  // block is worth. Splitting cannot exploit it either: the block counts here
  // and in GasCost::sha256_hash differ by at most two, so streaming never
  // undercuts the one-shot (Sha256GasModelTest pins that).
  return {SyscallErrorCode::kSyscallErrorCodeNone, required_gas};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_SHA256Copy_inner(SystemCallHandler &handler, uint32_t input_src_index,
                         uint32_t input_dst_index)
{
  auto r = handler.sha256_copy(input_src_index, input_dst_index);
  if (!r.has_value()) {
    return {r.error().code(), WrappedGasAmount{0}};
  }

  // Cloning a stream compresses nothing, so the stream is the whole cost.
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kSHA256StreamCost}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_SHA256Finalize_inner(SystemCallHandler &handler, uint32_t input_index,
                             SHA256Hash &output_output)
{
  auto r = handler.sha256_finalize(input_index);
  if (!r.has_value()) {
    return {r.error().code(), WrappedGasAmount{0}};
  }
  output_output = *r;
  if (auto *trace_data =
          handler.get_syscall_trace_data<SyscallNo::kSHA256Finalize>()) {
    trace_data->outputs.emplace(
        SyscallSHA256FinalizeOutputs{.hash = output_output});
  }
  // The padding block the streaming path has not compressed yet. Finalize
  // erases a stream rather than creating one, so it pays no stream cost.
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kSHA256HashBlockCost}};
}

// Verifies a cryptographic signature against a message using the provided
// public key
// Returns: Status code indicating if verification succeeded and gas cost
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_VerifySignature_inner(int32_t input_scheme,
                              std::span<const std::byte> input_signature,
                              std::span<const std::byte> input_public_key,
                              std::span<const std::byte> input_message)
{
  static_assert(std::same_as<decltype(input_scheme),
                             std::underlying_type_t<CryptographicScheme>>);

  switch (static_cast<CryptographicScheme>(input_scheme)) {
  case CryptographicScheme::Ed25519:
  {
    // Rejected on inspection, before any curve arithmetic: owes only the
    // crossing.
    if (input_public_key.size() != sizeof(Ed25519PublicKey) ||
        input_signature.size() != sizeof(Ed25519Signature)) {
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
              WrappedGasAmount{0}};
    }
    // Charged whether or not verification succeeds: ed25519 only exits early
    // when the encoded point fails to decompress, which the attacker choosing
    // the signature controls, so a free failure path sells verification for
    // the price of the crossing.
    const WrappedGasAmount gas{GasCost::verify_signature(input_message.size())};
    if (!ed25519_verify(input_public_key.subspan<0, sizeof(Ed25519PublicKey)>(),
                        input_signature.subspan<0, sizeof(Ed25519Signature)>(),
                        input_message)) {
      trace("Signature verification failed");
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, gas};
    }
    return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
  }
  default:
    // An unknown scheme never reaches a primitive.
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
}

} // namespace hogd
