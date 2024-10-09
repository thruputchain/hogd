% #include "types.hh"

    namespace hogd
{

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

  union StorageDeltaClass switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper nnin64_set_value; // int64_t
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
