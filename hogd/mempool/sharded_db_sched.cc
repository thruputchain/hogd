#include "hogd/mempool/sharded_db_sched.h"

#include "hogd/base/log.h"
#include "hogd/db_shard_routing/sharding.h" // shard_of_address

namespace hogd {

x23::Async<DbSchedResult>
ShardedDbSched::create_snapshot(BlockID prev_block_id, BlockID block_id,
                                BlockExecutionMode mode)
{
  co_return co_await shards_.broadcast_status(
      [&](auto &client) {
        return client->dbsched_create_snapshot(prev_block_id, block_id, mode);
      },
      DbSchedResult::DbSchedResultOK);
}

x23::Async<DbSchedResult>
ShardedDbSched::abandon_snapshot(BlockID block_id)
{
  co_return co_await shards_.broadcast_status(
      [&](auto &client) { return client->dbsched_abandon_snapshot(block_id); },
      DbSchedResult::DbSchedResultOK);
}

x23::Async<DbBlockResult>
ShardedDbSched::close_snapshot(BlockID block_id)
{
  DbBlockResult result =
      co_await shards_.primary()->dbsched_close_snapshot(block_id);
  for (size_t i = 1; i < shards_.shard_count(); ++i) {
    DbBlockResult r =
        co_await shards_.shard(i)->dbsched_close_snapshot(block_id);
    if (result.result() == DbSchedResult::DbSchedResultOK &&
        r.result() != DbSchedResult::DbSchedResultOK) {
      result = r;
    }
  }
  co_return result;
}

x23::Async<DbBlockResult>
ShardedDbSched::commit_snapshot(BlockID block_id, ConsensusValue value)
{
  DbBlockResult result =
      co_await shards_.primary()->dbsched_commit_snapshot(block_id, value);
  for (size_t i = 1; i < shards_.shard_count(); ++i) {
    DbBlockResult r =
        co_await shards_.shard(i)->dbsched_commit_snapshot(block_id, value);
    if (result.result() == DbSchedResult::DbSchedResultOK &&
        r.result() != DbSchedResult::DbSchedResultOK) {
      result = r;
    }
  }
  co_return result;
}

x23::Async<DbSchedResult>
ShardedDbSched::load_genesis(GenesisConfiguration genesis_configuration,
                             ConsensusValue value)
{
  for (size_t s = 0; s < shards_.shard_count(); ++s) {
    DbSchedResult ret =
        co_await shards_.shard(s)->dbsched_load_genesis_configuration(
            genesis_configuration, value);
    if (ret != DbSchedResult::DbSchedResultOK) {
      co_return ret;
    }
  }
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbTipResult>
ShardedDbSched::get_tip()
{
  // Read EVERY shard, never just shard 0. This is the validator-startup check
  // that a partially-committed cluster refuses to start rather than livelock:
  // commit fans out non-atomically, so a crash mid-fan-out can leave shards at
  // genuinely different tips, and create_snapshot(prev=tip) on a lagging shard
  // would then fail block after block. Two invariants, both fail-stop:
  //   (1) genesis presence is unanimous -- every shard has a committed tip or
  //       none do. Trusting shard 0 alone would let a wiped/replaced shard-0
  //       data dir report "no genesis" while other shards still hold committed
  //       state, and the caller would re-run the genesis fan-out against shards
  //       that already have it.
  //   (2) committed shards agree on (height, id, txs_hash). The storage hashes
  //       are legitimately per-shard (each shard's own root), so they are NOT
  //       checked.
  // Log before each CHECK so the abort is an actionable operator report rather
  // than two bare numbers.
  DbTipResult primary = co_await shards_.primary()->dbsched_get_tip();
  bool const primary_ok = primary.result() == DbSchedResult::DbSchedResultOK;
  for (size_t i = 1; i < shards_.shard_count(); ++i) {
    DbTipResult r = co_await shards_.shard(i)->dbsched_get_tip();
    bool const r_ok = r.result() == DbSchedResult::DbSchedResultOK;

    if (r_ok != primary_ok) {
      error("db shard genesis-presence divergence: shard 0 result {}, shard {} "
            "result {}; refusing to start",
            primary.result(), i, r.result());
      CHECK_EQ(r_ok, primary_ok);
    }
    if (!primary_ok) {
      continue; // both agree "no committed tip"; nothing more to reconcile
    }

    ProposalHeader const &r_header = r.db_tip().value.proposal_header;
    ProposalHeader const &primary_header =
        primary.db_tip().value.proposal_header;
    if (r_header.block_height != primary_header.block_height ||
        r.db_tip().id != primary.db_tip().id ||
        r_header.txs_hash != primary_header.txs_hash) {
      error("db shard tip divergence: shard {} at height {} (id {}), shard 0 at "
            "height {} (id {}); refusing to start",
            i, r_header.block_height, r.db_tip().id,
            primary_header.block_height, primary.db_tip().id);
    }
    CHECK_EQ(r_header.block_height, primary_header.block_height);
    CHECK_EQ(r.db_tip().id, primary.db_tip().id);
    CHECK_EQ(r_header.txs_hash, primary_header.txs_hash);
  }
  co_return primary;
}

x23::Async<DbSchedResult>
ShardedDbSched::write_hog_ages(HogAgeBatch const &ages)
{
  co_return co_await shards_.scatter_write(
      ages,
      [](HogAge const &a, size_t n) { return shard_of_address(a.address, n); },
      [](auto &c, auto &&sub) { return c->dbsched_write_hog_ages(sub); },
      DbSchedResult::DbSchedResultOK);
}

x23::Async<HogAgeResults>
ShardedDbSched::read_hog_ages(ContractAddressBatch const &accounts)
{
  co_return co_await shards_.scatter_gather<HogAgeResults>(
      accounts,
      [](ContractAddress const &a, size_t n) { return shard_of_address(a, n); },
      [](auto &c, auto &&sub) { return c->dbsched_get_hog_ages(sub); });
}

} // namespace hogd
