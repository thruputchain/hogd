// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  typedef opaque Bytestring<kBytestringMaxLen>;

  struct BoundedHashsetEntry {
    unsigned hyper threshold;
    SHA256Hash hash;
  };

  struct BoundedHashset {
    unsigned int max_size;
    BoundedHashsetEntry entries<>;
  };

  union StorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  case kBoundedHashset:
    BoundedHashset hashset;
  };

  // Things to do: figure out which info is stripped, when, and how that
  // is signalled to rpc caller
  union StrippedStorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  case kBoundedHashset:
    BoundedHashset hashset;
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

  union StorageDeltaClass switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper nnint64_set_value; // int64_t
  };

  enum StorageReserveResult {
    // reservation applied successfully
    kSuccess = 0,

    // Reservation definitely conflicts with an already committed transaction,
    // or is a mismatched type.
    kDefiniteFailure = 1,

    // Might succeed if retried later
    kMaybeConflict = 2
  };
} // namespace hogd
