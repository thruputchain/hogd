// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  enum StorageItemType {
    kBytestring = 0,
    kNonnegativeInt = 1,
    kDeleted = 2,
    kRetrocausalQueue = 3,
  };

  const kBytestringMaxLen = 1024;

  typedef opaque Bytestring<>;

  union StorageItem switch (StorageItemType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt:
    int128_xdr nnint;
  case kRetrocausalQueue:
    void; // TODO(geoff): figure out serialization format/hashing for
          // retrocausal queue
  };

  enum StorageDeltaType {
    kBytestringSet = 0,
    kNonnegativeIntSetAdd = 1,
    kDelete = 2,
    kRetrocausalQueueModify = 3,
  };

  struct NonnegativeInt128SetAddArgs {
    int128_xdr set_value;
    int128_xdr add_value;
  };

  typedef opaque QueueEntry[32];

  struct RetrocausalQueueBlock {
    QueueEntry entries<>;
    unsigned hyper block_number;
  };

  // This delta consumes all of the retrocausal queue entries
  // in blocks less than or equal to max_fully_consumed_block_number,
  // and removes the first partial_consumed_number entries from the
  // block number max_fully_consumed_block_number + 1.
  struct RetrocausalQueuePastBlockConsumeDelta {
    unsigned hyper max_fully_consumed_block_number;
    unsigned hyper partial_consumed_number;
  };

  struct RetrocausalQueueConsumeDelta {
    RetrocausalQueuePastBlockConsumeDelta past_block_consume;
    QueueEntry current_block_consumed<>;
  };

  struct RetrocausalQueueInsertDelta {
    QueueEntry added_entries<>;
  };

  struct RetrocausalQueueDelta {
    // set to null if no entries are consumed.
    // This version of the retrocausal queue is single-consumer multi-producer.
    RetrocausalQueueConsumeDelta *consume;
    RetrocausalQueueInsertDelta insert;
  };

  union StorageDelta switch (StorageDeltaType type)
  {
  case kBytestringSet:
    Bytestring bytestring;
  case kNonnegativeIntSetAdd:
    NonnegativeInt128SetAddArgs nnint;
  case kDelete:
    void;
  case kRetrocausalQueueModify:
    RetrocausalQueueDelta retrocausal_queue;
  };

  struct FlaggedStorageDelta {
    StorageDelta delta;
    StorageDeltaFlags flags;
  };

} // namespace hogd
