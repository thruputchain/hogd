#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
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

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BufferSHA256_inner(SystemCallHandler &syscall_handler,
                           const uint32_t &input_handle,
                           SHA256Hash &output_hash)
{
  auto status = syscall_handler.get_buffer(input_handle);
  if (!status) {
    return {status.error().code(), 0};
  }
  return syscall_SHA256_inner(*status, output_hash);
}

// Verifies a cryptographic signature against a message using the provided
// public key Returns: Status code indicating if verification succeeded and gas
// cost
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_VerifySignature_inner(Signature const &signature,
                              PublicKey const &public_key,
                              std::span<const std::byte> message)
{
  if (public_key.scheme() != signature.scheme()) {
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }
  switch (public_key.scheme()) {
  case CryptographicScheme::Ed25519:
    if (!ed25519_verify(public_key.ed25519(), signature.ed25519(), message)) {
      return {SyscallErrorCode::kInvalidSyscallInput, 0};
    }
    return {SyscallErrorCode::kNone, 0};
  default:
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }
}

} // namespace hogd
