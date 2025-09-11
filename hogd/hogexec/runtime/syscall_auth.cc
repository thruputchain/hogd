
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "account.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_ChangePublicKeyRegistration_inner(
    SystemCallHandler &syscall_handler, uint32_t input_key_index,
    const int32_t *optional_input_key_scheme,
    std::span<const std::byte> input_key_bytes,
    const uint32_t *optional_input_key_weight)
{
  auto new_key = [&] -> SyscallStatus<PublicKeyConfigOpt> {
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
    return {new_key.error().code(), 0};
  }

  auto status = syscall_handler.change_public_key_registration(input_key_index,
                                                               new_key.value());
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
