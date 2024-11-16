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

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  output_hash = sha256(input_buffer);
  return {SyscallErrorCode::kNone, SHA256HashCost(input_buffer.size())};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_VerifySignature_inner(const PublicKey &input_pk,
                              const Signature &input_sig,
                              std::span<const std::byte> input_message)
{
  if (input_pk.scheme() != input_sig.scheme()) {
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }

  using enum CryptographicScheme;

  switch (input_pk.scheme()) {
  case Ed25519:
    if (!ed25519_verify(input_pk.ed25519(), input_sig.ed25519(),
                        input_message)) {
      return {SyscallErrorCode::kInvalidSyscallInput, 0};
    }
    return {SyscallErrorCode::kNone, 0};
  default:
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }
}

} // namespace hogd
