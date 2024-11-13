// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  enum StorageObjectType {
    kBytestring = 0,
    kNonnegativeInt64 = 1,
  };

  const kBytestringMaxLen = 1024;

  typedef opaque Bytestring<kBytestringMaxLen>;

  union StorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  };

  enum StorageDeltaType {
    kDeleteLast = 0,
    kBytestringSet = 1,
    kNonnegativeInt64SetAdd = 2,
  };

  struct NonnegativeInt64SetAddArgs {
    hyper set_value; // int64_t
    hyper add_value; // int64_t
  };

  union StorageDelta switch (StorageDeltaType type)
  {
  case kDeleteLast:
    void;
  case kBytestringSet:
    Bytestring bytestring;
  case kNonnegativeInt64SetAdd:
    NonnegativeInt64SetAddArgs nnint64;
  };

} // namespace hogd
