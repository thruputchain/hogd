#pragma once

#include <cstdint>
#include <optional>

#include "hogd/hog_mempool/block_transfers.h"
#include "sched_db.hh"

namespace hogd {

// Reassembles the run of HogsTransferredChunk records hog_db sends for a
// committed block into one BlockTransfers, so the scheduler keeps seeing one
// update per height. hog_db sends a run in order, one chunk at a time, on one
// connection; the caller serializes push(). A chunk that breaks its run's
// framing is a hog_db bug and fail-stops. This assembles one shard's runs;
// with several shards (hogd#3108) each shard needs its own.
class TransferRunAssembler {
public:
  // Moves [chunk]'s deltas into the pending block. Returns the block once its
  // last chunk is in, otherwise nullopt.
  std::optional<BlockTransfers> push(HogsTransferredChunk chunk);

private:
  struct Pending {
    BlockTransfers block;
    std::uint32_t chunk_count;
  };
  std::optional<Pending> pending_;
};

} // namespace hogd
