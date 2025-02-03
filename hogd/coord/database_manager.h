#pragma once

#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include <vector>

namespace hogd {

class DatabaseManager {
public:
  using database_client_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  void add_database_shard(database_client_t &&client);

  // close a block to new transactions.  Does not commit block to disk,
  // nor does it prevent sibling blocks from being opened.
  x23::Async<bool> close_block(BlockID block_id);

  // commit a block to disk.  This block must be the immediate descendant
  // of the most recently committed block.
  // Prevents sibling blocks from being opened.
  x23::Async<void> commit_block(BlockID block_id);

  x23::Async<void> open_block(BlockID parent_id, BlockID block_id);

private:
  std::vector<database_client_t> database_clients_;
};

} // namespace hogd
