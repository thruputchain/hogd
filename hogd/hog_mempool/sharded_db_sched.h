#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "db_sched.hh"
#include "genesis.hh"
#include "rpc_types.hh"
#include "sched_db.hh" // ShardCard
#include "types.hh"

#include "hogd/db_shard_routing/sharded_client.h"
#include "hogd/mempool/shared.h" // db_sched_client_t
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// Reduce the shards' block results to the one an unsharded db would return:
// first non-OK in shard order wins, otherwise the db hash is the sum of the
// shards' partial LtHash16 states (see ShardedDbSched below). Exposed for
// testing; callers should go through close_snapshot/commit_snapshot.
DbBlockResult combine_block_results(std::vector<DbBlockResult> const &results);

// The mempool's sole handle to the sharded db: one client per shard, exposing
// only the operations the mempool uses, each fixing that RPC's sharding policy.
// Adding an RPC means adding a method here, routed to match the key layout in
// sharding.h.
//
// Keys with an address field scatter to shard_of_address(address); reads gather
// back into input order. Only 'A' hog ages here; hogexec owns the rest.
// (write_hog_ages, read_hog_ages)
//
// 'C' code keys carry a content hash instead, so they live on shard 0 alone --
// nothing here; hogexec's DatabaseProxy owns them.
//
// '<' keys are not sharded: every shard keeps its own globals and runs the
// whole block lifecycle, validating create_snapshot(prev) against its own
// chain. Writes go to every shard; reads require agreement, which turns a shard
// that diverged (a crash mid-commit-fan-out) into a loud startup failure
// instead of a livelock. (create/abandon/close/commit_snapshot, load_genesis,
// get_tip)
//
// db_hash is the exception, legitimately per-shard: a shard hashes only the
// items it owns, and those are LtHash16 multiset values, so this class sums the
// partials and hands out the whole-db hash. Only the DbTip each shard persists
// holds that shard's partial.
//
// Viewing snapshot ids are not keys, so they have no layout to follow: each
// hogdb mints its own, and a cross-shard operation holds one id per shard.
// Nothing here yet.
class ShardedDbSched {
public:
  explicit ShardedDbSched(
      std::vector<std::shared_ptr<db_sched_client_t>> clients)
    : shards_(std::move(clients))
  {}

  // Connect one client per card, bound by declared shard_id (fail-stop on a bad
  // identity set -- see order_shards_by_id). The one place identity is enforced
  // mempool-side.
  static x23::Async<std::shared_ptr<ShardedDbSched>>
  connect(x23::XdrVec<ShardCard> cards);

  size_t shard_count() const { return shards_.shard_count(); }

  // Broadcast to every shard; first non-OK wins.
  x23::Async<DbSchedResult> create_snapshot(BlockID prev_block_id,
                                            BlockID block_id,
                                            BlockExecutionMode mode);
  x23::Async<DbSchedResult> abandon_snapshot(BlockID block_id);

  // Fan out to every shard; first non-OK wins. The returned db hash is the sum
  // of the shards' partials, so the block header carries the value an unsharded
  // db would produce. commit is not atomic under partial failure (see the
  // cross-shard TODO(sharding) notes).
  x23::Async<DbBlockResult> close_snapshot(BlockID block_id);
  x23::Async<DbBlockResult> commit_snapshot(BlockID block_id,
                                            ConsensusValue const &value);

  // Fan out in shard order, stopping at the first non-OK. The cross-shard
  // readiness barrier is the caller's concern.
  x23::Async<DbSchedResult>
  load_genesis(GenesisConfiguration genesis_configuration,
               ConsensusValue value);

  // Read every shard's tip and fail-stop on divergence. Returns the replicated
  // tip with storage_long_hash replaced by the sum of the shards' partials --
  // so DbTip.storage_long_hash means "this shard's items" inside hogdb and
  // "the whole db" everywhere above here.
  x23::Async<DbTipResult> get_tip();

  // Scatter by account address; reads gather back into input order.
  x23::Async<DbSchedResult> write_hog_ages(HogAgeBatch const &ages);
  x23::Async<HogAgeResults> read_hog_ages(ContractAddressBatch const &accounts);

private:
  ShardedClient<std::shared_ptr<db_sched_client_t>> shards_;
};

} // namespace hogd
