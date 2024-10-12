// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  const kBytestringMaxLen = 1000;


  typedef opaque Bytestring<kBytestringMaxLen>;

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

  struct StorageDeltaVec {
    AddressAndKey addr_key;
    StorageDelta deltas<>;
  };

  typedef StorageDeltaVec StorageDeltaVecList<>;

} // namespace hogd
