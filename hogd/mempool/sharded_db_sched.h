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
// Adding an RPC means adding a method here, and classifying it below.
//
// Every db RPC is one of four kinds. A read must route the way the write that
// produced its data routed, so pick the kind before writing the method.
//
//   1. Keyed state -- contract data, balances, auth data, RCQ entries, hog
//      ages. Scattered to the owning shard, shard_of_address(address), and
//      gathered back into input order. (write_hog_ages, read_hog_ages)
//
//   2. Content-addressed state -- wasm code, keyed by hash, not address.
//      Shard 0 only. Nothing here yet; hogexec's DatabaseProxy owns it.
//
//   3. Replicated control state -- the block lifecycle and genesis. Written to
//      every shard; each shard runs its own copy of the chain and validates
//      create_snapshot(prev) against it, so the shards must advance in
//      lockstep. Reads consult every shard and require agreement, which turns
//      a shard that diverged (a crash mid-commit-fan-out) into a loud startup
//      failure instead of a livelock. (create/abandon/close/commit_snapshot,
//      load_genesis, get_tip)
//
//   4. Per-shard-minted state -- viewing snapshot ids, which each hogdb
//      generates locally, so a cross-shard operation holds one id per shard.
//      Nothing here yet.
//
// The one value that is legitimately per-shard is the db hash: a shard hashes
// only the items it owns. Those are LtHash16 multiset values, so this class
// sums the partials and hands out the whole-db hash; only the DbTip each shard
// persists holds that shard's partial.
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
                                            ConsensusValue value);

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
