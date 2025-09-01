#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

// Computes the SHA256 hash of the input buffer and returns the gas cost
// Returns: Status code and gas cost for the operation
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(SystemCallHandler &handler,
                     std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  output_hash = sha256(input_buffer);

  if (auto *stats = handler.get_transaction_stats()) {
    stats->sha256_total_bytes_processed += input_buffer.size();
  }
  return {SyscallErrorCode::kNone, SHA256HashCost(input_buffer.size())};
};

// Verifies a cryptographic signature against a message using the provided
// public key
// Returns: Status code indicating if verification succeeded and gas cost
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_VerifySignature_inner(int32_t scheme,
                              std::span<const std::byte> signature,
                              std::span<const std::byte> public_key,
                              std::span<const std::byte> message)
{
  static_assert(std::same_as<decltype(scheme),
                             std::underlying_type_t<CryptographicScheme>>);

  switch (static_cast<CryptographicScheme>(scheme)) {
  case CryptographicScheme::Ed25519:
    if (public_key.size() != sizeof(Ed25519PublicKey) ||
        signature.size() != sizeof(Ed25519Signature)) {
      return {SyscallErrorCode::kInvalidSyscallInput, 0};
    }
    if (!ed25519_verify(public_key.subspan<0, sizeof(Ed25519PublicKey)>(),
                        signature.subspan<0, sizeof(Ed25519Signature)>(),
                        message)) {
      trace("Signature verification failed");
      return {SyscallErrorCode::kInvalidSyscallInput, 0};
    }
    return {SyscallErrorCode::kNone, 0};
  default:
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }
}

} // namespace hogd
