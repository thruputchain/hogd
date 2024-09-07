#include "hogd/syscall/syscall_status.h"

namespace hogd
{


std::string
SyscallErrorCategory::message(int code) const
{
  using enum SyscallErrorCode;
  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int>, 
  	"Slightly too strong, but potential for UB or dropped errors otherwise (CWG defects 1766)");

  switch (static_cast<SyscallErrorCode>(code)) {
  case kIsRootTransaction:
  	return "kIsRootTransaction";
  case kStorageObjectTypeMismatch:
  	return "kStorageObjectTypeMismatch";
  case kStorageKeyNExist:
  	return "kStorageKeyNExist";
  case kStorageKeyAlreadyDeleted:
  	return "kStorageKeyAlreadyDeleted";

  case kNonnegativeInt64SetValueMismatch:
  	return "kNonnegativeInt64SetValueMismatch";

  case kDoubleReturn:
  	return "kDoubleReturn";

  case kInvalidPtrError:
  	return "kInvalidPtrError";

  case kUnrecoverableUnknown:
  	return "kUnrecoverableUnknown";
  }
  return "unknown";
}


bool
SyscallError::recoverable() const {
	return (std::to_underlying(code()) >= 0);
}

}