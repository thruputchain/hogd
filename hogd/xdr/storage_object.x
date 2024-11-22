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

  const kMessageQueueEventMaxLen = 1024;

  // I would like to call this a "Message" but that's overloaded with many other
  // things
  typedef opaque MessageQueueEvent<kMessageQueueEventMaxLen>;

  struct MessageQueue {
    // All the new events in a block are sorted lexicographically,
    // then put at the back of the list.
    MessageQueueEvent events<>;
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
  case kMessageQueue:
    MessageQueue message_queue;
  case kDeleted:
    void;
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
  case kMessageQueue:
    MessageQueue message_queue;
  };

  union StorageObjectTypeClass switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  default:
    void;
  };

  enum StorageLockType {
    kNone = 0,
    kSetObjectTypeClass = 1, // kind of like a readlock
    kExclusiveLock = 2,
  };

  union TypeSpecificLock switch(StorageObjectType object_type) {
    case kNonnegativeInt64:
      hyper nnint64_subtracted_amount;
    default:
      void;
  };

  // data sent to db servers about storage lock state
  union StorageLock switch (StorageLockType lock_type) {
    case kSetObjectTypeClass:
      TypeSpecificLock extra_lock_data;
    case kExclusiveLock:
      void;
    // is never instantiated for kNone
  };

  struct FullSetObjectTypeClassLock {
    StorageObjectTypeClass typeclass;
    TypeSpecificLock extra_lock_data;
  };

  // data tracked in hogexec about storage lock state
  union FullStorageLock switch (StorageLockType lock_type) {
    case kSetObjectTypeClass:
      FullSetObjectTypeClassLock full_typeclass_lock;
    case kExclusiveLock:
      void;
    // is never instantiated for kNone
  };

  // Possible transitions:
  // kNone can only move to kSetObjectTypeClass
  // kSetObjectTypeClass can go to any state,
  // but if moving to the same state,
  // the only thing that can change is the type_specific_extra field
  // kExclusive can move to any other state

  struct StorageLockChange {
    StorageLockType previous_type;
    StorageLockType new_type;

    // only set if previous_type != new_type, and not the exclusive -> none
    // transition
    StorageObjectTypeClass *new_typeclass;

    union switch (StorageObjectType type)
    {
    case kNonnegativeInt64:
      hyper marginal_nnint64_subtracted_amount;
    default:
      void;
    }
    type_specific_extra;
  };

  enum StorageDeltaType {
    kDelete = 0,
    kBytestringSet = 1,
    kNonnegativeInt64SetAdd = 2,
    kReplayCacheInsert = 3,
    kReplayCacheClear = 4,
    kMessageQueueAppend = 5,
    kMessageQueueConsume = 6,
  };

  struct NonnegativeInt64SetAddArgs {
    hyper set_value; // int64_t
    hyper add_value; // int64_t
  };

  union StorageDelta switch (StorageDeltaType type)
  {
  case kDelete:
    void;
  case kBytestringSet:
    Bytestring bytestring;
  case kNonnegativeInt64SetAdd:
    NonnegativeInt64SetAddArgs nnint64;
  case kReplayCacheInsert:
    unsigned hyper replay_nonces<>; // uint64_t<>
  case kReplayCacheClear:
    unsigned hyper replay_clear_threshold; // uint64_t
  case kMessageQueueAppend:
    MessageQueueEvent append_event;
  case kMessageQueueConsume:
    unsigned int num_messages_consumed;
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
