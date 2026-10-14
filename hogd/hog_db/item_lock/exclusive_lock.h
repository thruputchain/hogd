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
  // Returns the cost of storing the after change, which must not be nullptr.
  // The before parameter is ignored; it keeps the signature of the other lock
  // types. Fails only where the typed lock's admission would fail too (an
  // invalid queue delta, a duplicate signer), so that a transaction is refused
  // when it reserves rather than when it commits.
  HogDbStatus<int128_t> try_atomic_change_lock(const ItemChange *before,
                                               const ItemChange *after,
                                               const LockContext &ctx);

private:
};

} // namespace hogd
