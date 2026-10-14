#pragma once

#include "hogd/base/chunk_run_assembler.h"
#include "hogd/base/chunked_vector.h"
#include "sched_db.hh"
#include "types.hh"

namespace hogd {

// The deltas of one HogsTransferredChunk, as x23 decodes them.
using DeltaChunk = decltype(HogsTransferredChunk::deltas);

// Reassembles a block's run of HogsTransferredChunk payloads.
using TransferRunAssembler = ChunkRunAssembler<BlockHeight, DeltaChunk>;

// A committed block's native-token balance deltas, reassembled from the run of
// chunks hog_db sent for it. The chunks stay the separate vectors x23 decoded,
// moved in as they arrived, so reassembly never copies a delta; [deltas] reads
// them as one sequence.
struct BlockTransfers {
  BlockHeight block;
  ChunkedVector<DeltaChunk> deltas;
};

} // namespace hogd
