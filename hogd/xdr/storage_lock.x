// clang-format off
%#include "types.hh"
%#include "storage_item.hh"

namespace hogd
{
  // clang-format on

  union StorageItemTypeClass switch (StorageItemType item_type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt:
    int128_xdr nnint_set;
  default:
    void;
  };

  enum StorageLockType {
    kNone = 0,
    kSetItemTypeClass = 1, // kind of like a readlock
    kExclusiveLock = 2,
    kPermanentExclusiveLock = 3,
  };

  union TypeSpecificLock switch (StorageItemType item_type)
  {
  case kNonnegativeInt:
    int128_xdr nnint_subtracted_amount;
  case kRetrocausalQueue:
    unsigned int consume_lock;
  default:
    void;
  };

  struct SetItemTypeClassLock {
    StorageItemTypeClass typeclass;
    TypeSpecificLock extra_lock_data;
  };

  // TODO(geoff): This could be stripped down,
  // and/or cached in the executor as necessary.
  union StorageLock switch (StorageLockType lock_type)
  {
  case kNone:
    void;
  case kSetItemTypeClass:
    SetItemTypeClassLock full_typeclass_lock;
  case kExclusiveLock:
    void;
  case kPermanentExclusiveLock:
    void;
  };

} // namespace hogd
