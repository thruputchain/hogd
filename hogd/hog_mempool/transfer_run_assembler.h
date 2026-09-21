#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "hogd/hog_mempool/block_transfers.h"
#include "sched_db.hh"

namespace hogd {

// Reassembles the run of HogsTransferredChunk records hog_db sends for a
// committed block into one BlockTransfers, so the scheduler keeps seeing one
// update per height. A run's chunks may arrive in any order and on any
// connection: each lands in its chunk_index slot, and the run completes once
// every slot is filled. hog_db starts the next block's run only after every
// chunk of this one is acknowledged, so at most one run is pending; the
// caller serializes push(). A chunk that breaks its run's framing (another
// block while one is pending, a count that changes, an index seen twice) is a
// hog_db bug and fail-stops. This assembles one shard's runs; with several
// shards (hogd#3108) each shard needs its own.
class TransferRunAssembler {
public:
  // Moves [chunk]'s deltas into their slot of the pending block. Returns the
  // block once every chunk of its run is in, otherwise nullopt.
  std::optional<BlockTransfers> push(HogsTransferredChunk chunk);

private:
  struct Pending {
    BlockTransfers block;
    std::vector<bool> received;
    std::uint32_t remaining = 0;
  };
  std::optional<Pending> pending_;
};

} // namespace hogd
