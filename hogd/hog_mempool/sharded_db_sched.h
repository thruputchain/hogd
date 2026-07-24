#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "db_sched.hh"
#include "genesis.hh"
#include "rpc_types.hh"
#include "types.hh"

#include "hogd/db_shard_routing/sharded_client.h"
#include "hogd/mempool/shared.h" // db_sched_client_t
#include "x23/async.h"

namespace hogd {

// The mempool's sole handle to the sharded database: owns one db-sched client
// per shard and exposes only the operations the mempool uses, each a named
// method fixing that RPC's sharding policy in one place. No generic
// pass-through -- adding an RPC means adding a method here. Delegates to
// ShardedClient; deliberately not a mirror of the generated DbSched1 interface.
class ShardedDbSched {
public:
  explicit ShardedDbSched(
      std::vector<std::shared_ptr<db_sched_client_t>> clients)
    : shards_(std::move(clients))
  {}

  size_t shard_count() const { return shards_.shard_count(); }

  // Broadcast to every shard; first non-OK wins.
  x23::Async<DbSchedResult> create_snapshot(BlockID prev_block_id,
                                            BlockID block_id,
                                            BlockExecutionMode mode);
  x23::Async<DbSchedResult> abandon_snapshot(BlockID block_id);

  // Fan out to every shard; the block's db_hash is shard 0's alone, first
  // non-OK wins. commit is not atomic under partial failure (see 2PC TODOs).
  x23::Async<DbBlockResult> close_snapshot(BlockID block_id);
  x23::Async<DbBlockResult> commit_snapshot(BlockID block_id,
                                            ConsensusValue value);

  // Fan out in shard order, stopping at the first non-OK. The cross-shard
  // readiness barrier is the caller's concern.
  x23::Async<DbSchedResult>
  load_genesis(GenesisConfiguration genesis_configuration,
               ConsensusValue value);

  // Read every shard's tip and require unanimous height/id (a divergence is a
  // CHECK), returning shard 0's DbTip. A missing shard-0 tip is returned as-is.
  x23::Async<DbTipResult> get_tip();

  // Scatter by account address; reads gather back into input order.
  x23::Async<DbSchedResult> write_hog_ages(HogAgeBatch ages);
  x23::Async<HogAgeResults> read_hog_ages(ContractAddressBatch const &accounts);

private:
  ShardedClient<std::shared_ptr<db_sched_client_t>> shards_;
};

} // namespace hogd
