#pragma once

#include <vector>

#include "sched_db.hh"
#include "types.hh"

namespace hogd {

// The deltas of one HogsTransferredChunk, as x23 decodes them.
using DeltaChunk = decltype(HogsTransferredChunk::deltas);

// A committed block's native-token balance deltas, reassembled from the run of
// chunks hog_db sent for it. The chunks stay separate vectors, moved in as they
// arrived, so reassembly never copies a delta; consumers iterate the nesting.
struct BlockTransfers {
  BlockHeight block;
  std::vector<DeltaChunk> delta_chunks;
};

} // namespace hogd
