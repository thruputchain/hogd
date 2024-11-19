// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  enum StorageObjectType {
    kBytestring = 0,
    kNonnegativeInt128 = 1,
  };

  const kBytestringMaxLen = 1024;

  typedef opaque Bytestring<kBytestringMaxLen>;

  union StorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt128:
    int128_xdr nnint128;
  };

  enum StorageDeltaType {
    kDeleteLast = 0,
    kBytestringSet = 1,
    kNonnegativeInt128SetAdd = 2,
  };

  struct NonnegativeInt128SetAddArgs {
    int128_xdr set_value;
    int128_xdr add_value;
  };

  union StorageDelta switch (StorageDeltaType type)
  {
  case kDeleteLast:
    void;
  case kBytestringSet:
    Bytestring bytestring;
  case kNonnegativeInt128SetAdd:
    NonnegativeInt128SetAddArgs nnint128;
  };

} // namespace hogd
