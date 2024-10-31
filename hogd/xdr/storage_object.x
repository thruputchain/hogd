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
    unsigned int max_size; // uint32_t
    BoundedHashsetEntry entries<>;
  };

  struct ReplayCacheMetadata {
    unsigned hyper clear_threshold; // uint64_t
    unsigned int current_size;      // uint32_t
  };

  struct ReplayCache {
    ReplayCacheMetadata metadata;

    // metadata.current_size == nonces.size()
    unsigned hyper nonces<>;
  };

  union StorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  case kBoundedHashset:
    BoundedHashset hashset;
  case kReplayCache:
    ReplayCache replay_cache;
  };

  // Things to do: figure out which info is stripped, when, and how that
  // is signalled to rpc caller (for each type)
  union StrippedStorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  case kBoundedHashset:
    BoundedHashset hashset;
  case kReplayCache:
    ReplayCacheMetadata replay_cache_metadata;
  };

  enum StorageDeltaType {
    kDeleteLast = 0,
    kBytestringSet = 1,
    kNonnegativeInt64SetAdd = 2,
    kReplayCacheInsert = 3,
    kReplayCacheClear = 4,
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
  case kReplayCacheInsert:
    unsigned hyper replay_nonces<>; // uint64_t<>
  case kReplayCacheClear:
    unsigned hyper replay_clear_threshold; // uint64_t
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
