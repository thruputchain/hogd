#include "hogd/syscall/syscall_status.h"

#include <string>

#include "hogd/storage_proxy/types.h"

namespace hogd {

std::string
SyscallErrorCategory::message(int code) const
{
  using enum SyscallErrorCode;

  switch (static_cast<SyscallErrorCode>(code)) {
  case kStorageKeyNExist:
    return "kStorageKeyNExist";
  case kStorageKeyAlreadyDeleted:
    return "kStorageKeyAlreadyDeleted";
  }
  return "unknown";
}

} // namespace hogd
