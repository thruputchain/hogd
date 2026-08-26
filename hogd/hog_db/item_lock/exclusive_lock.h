#pragma once

#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"

namespace hogd {

// The exclusive lock is held if and only if there exists one transaction with
// multiple (conflicting) reservations for the same item. This lock state will
// never commit to the db directly.  Instead, over the course of a transaction,
// if an ItemLock is placed into this state, it will eventually be converted
// back into one of the other lock states\ by an unlock or by a commit_change.
class ExclusiveLock {
public:
  // Returns the cost of storing the after change. Fails when the change is
  // semantically invalid under an exclusive lock -- e.g. an RCQ delta whose
  // consumes don't reconcile, or a retrocausal-queue deletion whose block-close
  // cleanup would exceed kMaxRetrocausalQueueBlocksPerDeletion. The before
  // parameter is ignored; the signature matches the other lock types.
  HogDbStatus<int128_t> try_atomic_change_lock(const ItemChange *before,
                                               const ItemChange *after,
                                               const LockContext &ctx);

private:
};

} // namespace hogd
