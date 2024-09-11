#include "hogd/syscall/proxy_applicator.h"

#include "hogd/syscall/key_status.h"
#include "hogd/syscall/syscall_status.h"

namespace hogd {

KeyStatus
ProxyApplicator::has_key() const
{
  if (is_deleted_) {
    return KeyStatus::kKeyAlreadyDeleted;
  }
  // exists case unimplemented, pending x23c
  return KeyStatus::kKeyNExist;
}

SyscallStatus<void>
ProxyApplicator::delete_last()
{
  if (is_deleted_) {
    return SyscallError{SyscallErrorCode::kStorageKeyAlreadyDeleted};
  }
  is_deleted_ = true;
  return {};
}

} // namespace hogd
