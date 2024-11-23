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
    kBytestringSet = 0,
    kNonnegativeInt128SetAdd = 1,
  };

  struct NonnegativeInt128SetAddArgs {
    int128_xdr set_value;
    int128_xdr add_value;
  };

  union StorageDelta switch (StorageDeltaType type)
  {
  case kBytestringSet:
    Bytestring bytestring;
  case kNonnegativeInt128SetAdd:
    NonnegativeInt128SetAddArgs nnint128;
  };

} // namespace hogd
