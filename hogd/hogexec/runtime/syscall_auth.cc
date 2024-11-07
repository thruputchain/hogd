#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/crypto/crypto.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthPayloadHash_inner(SystemCallHandler &syscall_handler,
                                 const SHA256Hash *optional_input_message,
                                 SHA256Hash &output_auth_payload_hash)
{
  auto status = syscall_handler.get_auth_payload(optional_input_message);
  if (!status) {
    return {status.error().code(), 0};
  }

  auto buffer = x23::serialize(*status);

  output_auth_payload_hash = sha256(std::as_bytes(std::span(buffer)));

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_PushAuthContext_inner(SystemCallHandler &syscall_handler,
                              std::span<const std::byte> input_context)
{
  syscall_handler.push_auth_context(input_context);

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_PopAuthContext_inner(SystemCallHandler &syscall_handler)
{
  auto status = syscall_handler.pop_auth_context();

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RequireAuth_inner(SystemCallHandler &syscall_handler,
                          const ContractAddress &input_authorizing_address,
                          std::span<const std::byte> input_action)
{
  auto status =
      syscall_handler.require_auth(input_authorizing_address, input_action);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthWitness_inner(SystemCallHandler &syscall_handler,
                             const uint32_t input_witness_index,
                             std::span<std::byte> output_witness,
                             RuntimeMemoryLength &output_written_len)
{
  auto witness = syscall_handler.get_auth_witness(input_witness_index);

  if (!witness) {
    return {witness.error().code(), 0};
  }

  RuntimeMemoryLength write_len =
      std::min<RuntimeMemoryLength>(output_witness.size(), witness->size());

  std::memcpy(reinterpret_cast<uint8_t *>(output_witness.data()),
              witness->data(), write_len);

  output_written_len = write_len;

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthWitnessLen_inner(SystemCallHandler &syscall_handler,
                                const uint32_t input_witness_index,
                                RuntimeMemoryLength &output_witness_len)
{
  auto witness = syscall_handler.get_auth_witness(input_witness_index);

  if (!witness) {
    return {witness.error().code(), 0};
  }

  output_witness_len = witness->size();

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
