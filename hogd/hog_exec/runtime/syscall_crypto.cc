#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/mldsa.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "hogd/rust/crypto/bindings.h"
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

// Experimental API; encoding and pricing may change before stabilization.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Ristretto255Msm_inner(SystemCallHandler &handler,
                              WrappedGasAmount remaining_gas,
                              std::span<const std::byte> input_terms,
                              Ristretto255Point &output_point)
{
  if (input_terms.size() % RISTRETTO255_MSM_TERM_BYTES != 0 ||
      input_terms.size() / RISTRETTO255_MSM_TERM_BYTES >
          RISTRETTO255_MSM_MAX_TERMS) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  const WrappedGasAmount gas{GasCost::ristretto255_msm(
      input_terms.size() / RISTRETTO255_MSM_TERM_BYTES)};
  // Reserve decoding, allocation and arithmetic before entering Rust.
  // Exact exhaustion faults before entering Rust too.
  if (remaining_gas.get() <= gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  static_assert(sizeof(output_point) == 32);
  const MessageSpan terms{
      .message_ptr = reinterpret_cast<const uint8_t *>(input_terms.data()),
      .message_len = input_terms.size(),
  };
  // The parser owns this staged output. The Rust kernel writes it only after
  // every encoding has passed validation; no second staging buffer is needed.
  if (!ristretto255_msm(terms, reinterpret_cast<uint8_t (*)[32]>(
                                   output_point.data()))) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, gas};
  }
  if (auto *trace_data =
          handler.get_syscall_trace_data<SyscallNo::kRistretto255Msm>()) {
    trace_data->outputs.emplace(
        SyscallRistretto255MsmOutputs{.point = output_point});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

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
    // A failed lookup still probed the stream table, so charge for the probe
    // (#4218).
    return {r.error().code(), WrappedGasAmount{kSHA256StreamCost}};
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
    // A failed lookup still probed the stream table, so charge for the probe
    // (#4218).
    return {r.error().code(), WrappedGasAmount{kSHA256StreamCost}};
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
    // A failed lookup still probed the stream table, so charge for the probe
    // (#4218).
    return {r.error().code(), WrappedGasAmount{kSHA256StreamCost}};
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
syscall_VerifySignature_inner(WrappedGasAmount remaining_gas,
                              int32_t input_scheme,
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
    // Reserve the length-priced cost before running the verifier, so a low-gas
    // frame cannot get the curve work for the price of the crossing (#4172).
    const WrappedGasAmount gas{GasCost::verify_signature(input_message.size())};
    if (remaining_gas.get() <= gas.get()) {
      return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
    }
    // Charged whether or not verification succeeds: ed25519 only exits early
    // when the encoded point fails to decompress, which the attacker choosing
    // the signature controls, so a free failure path sells verification for
    // the price of the crossing.
    if (!ed25519_verify(input_public_key.subspan<0, sizeof(Ed25519PublicKey)>(),
                        input_signature.subspan<0, sizeof(Ed25519Signature)>(),
                        input_message)) {
      trace("Signature verification failed");
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, gas};
    }
    return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
  }
  case CryptographicScheme::MlDsa65:
  {
    // Rejected on inspection, before any lattice arithmetic: owes only the
    // crossing.
    if (input_public_key.size() != sizeof(MlDsa65PublicKey) ||
        input_signature.size() != sizeof(MlDsa65Signature)) {
      return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
              WrappedGasAmount{0}};
    }
    // Priced on its own (see kVerifySignatureMlDsa65BaseCost) and reserved
    // before verifying, as for ed25519 (#4172).
    const WrappedGasAmount gas{
        GasCost::verify_signature_mldsa65(input_message.size())};
    if (remaining_gas.get() <= gas.get()) {
      return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
    }
    // Charged whether or not verification succeeds. A malformed hint or an
    // oversized response vector does end ML-DSA verification early, but only
    // after it has hashed the key, and the attacker choosing the signature
    // controls that path, so charging less for it would sell part of the work
    // for the price of the crossing. The verifier takes the protocol's
    // fixed-size key and signature types, so the guest's bytes are copied into
    // them rather than aliased.
    MlDsa65PublicKey public_key;
    MlDsa65Signature signature;
    std::memcpy(public_key.data(), input_public_key.data(), public_key.size());
    std::memcpy(signature.data(), input_signature.data(), signature.size());
    // Pure ML-DSA with an empty context, as transaction authentication uses.
    if (!mldsa65_verify(public_key, signature, input_message)) {
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
