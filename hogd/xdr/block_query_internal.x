// clang-format off
%#include "consensus.hh"
%#include "pagination.hh"

namespace hogd
{
// clang-format on

// Private indexer storage. The database's block-query format marker gates
// decoding by storage format version, including ConsensusValue encoding.
// Wire protocol pin changes alone do not change this storage version.
struct BlockQueryRecord {
  ConsensusValue value;
  unsigned hyper transaction_count;
};

// Distinct from event/address cursors. The block hash binds the cursor to
// immutable content, including across a chain reset at the same height.
struct BlockQueryCursor {
  unsigned int format;
  BlockHeight height;
  ConsensusValueHash block_hash;
  IterationOrder order;
  TransactionHash last_hash;
};

} // namespace hogd
