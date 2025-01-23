#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

// Computes the SHA256 hash of the input buffer and returns the gas cost
// Returns: Status code and gas cost for the operation
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  output_hash = sha256(input_buffer);
  return {SyscallErrorCode::kNone, SHA256HashCost(input_buffer.size())};
};

// Verifies a cryptographic signature against a message using the provided
// public key
// Returns: Status code indicating if verification succeeded and gas cost
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_VerifySignature_inner(Signature const &signature,
                              PublicKey const &public_key,
                              std::span<const std::byte> message)
{
  if (public_key.scheme() != signature.scheme()) {
    return {SyscallErrorCode::kInvalidInput, 0};
  }
  switch (public_key.scheme()) {
  case CryptographicScheme::Ed25519:
    if (!ed25519_verify(public_key.ed25519(), signature.ed25519(), message)) {
      return {SyscallErrorCode::kInvalidInput, 0};
    }
    return {SyscallErrorCode::kNone, 0};
  default:
    return {SyscallErrorCode::kInvalidInput, 0};
  }
}

} // namespace hogd
