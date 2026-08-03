#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/base/log.h"
#include "rpc_types.hh" // BlockID, TransactionID, RPCError

namespace hogd {

// Pop `count` subtransactions from every shard in `clients`, visiting ALL of
// them even after a pop fails. Stopping early would leave the later shards one
// savepoint frame deeper than the rest, so the next try_push_new_subtransaction
// would hit them at the wrong depth. Returns the first non-`kRPCNone` error
// (`kRPCNone` if every pop succeeded) and logs each failure with its shard
// index. `Client` is anything exposing pop_subtransactions(BlockID,
// TransactionID, uint32_t) -> RPCError.
template<typename Client>
RPCError
pop_from_shards(std::span<Client> clients, BlockID block_id,
                TransactionID tx_id, uint32_t count)
{
  RPCError first_error = RPCError::kRPCNone;
  size_t shard = 0;
  for (Client &client : clients) {
    RPCError err = client.pop_subtransactions(block_id, tx_id, count);
    if (err != RPCError::kRPCNone) {
      error("failed to pop subtransaction on shard {}: {}", shard, err);
      if (first_error == RPCError::kRPCNone) {
        first_error = err;
      }
    }
    ++shard;
  }
  return first_error;
}

} // namespace hogd
