#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <sys/types.h>

#include "hogd/hog_mempool/scheduler.h"

namespace hogd {

// Checks that every cached tx is on its account's runqueue except the
// [nominated] txs that [Scheduler::schedule()] took off it and that have not
// been reenqueued or removed since. A cache entry that is in neither place is
// stranded: nothing will schedule it or remove it. Call this only between
// scheduler operations; see [Scheduler::size()].
inline ::testing::AssertionResult
cache_matches_runqueue(Scheduler &scheduler, size_t nominated)
{
  const size_t cached = scheduler.tx_cache_size();
  const ssize_t queued = scheduler.size();
  if (queued >= 0 && cached == static_cast<size_t>(queued) + nominated) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "tx cache holds " << cached << " txs, but the runqueue holds "
         << queued << " and the nomination " << nominated;
}

} // namespace hogd
