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
  DbTipResult primary = co_await shards_.primary()->dbsched_get_tip();
  if (primary.result() != DbSchedResult::DbSchedResultOK) {
    co_return primary; // no committed genesis yet; nothing to reconcile
  }
  // Shards commit in lockstep, so height/id must match. db_hash legitimately
  // differs per shard, so it is not part of the check.
  for (size_t i = 1; i < shards_.shard_count(); ++i) {
    DbTipResult r = co_await shards_.shard(i)->dbsched_get_tip();
    // Log before the CHECK: a tip divergence is the most important line the
    // operator will see, and CHECK_EQ alone prints two bare numbers with no
    // context. This turns "mempool aborted" into an actionable report.
    if (r.result() != DbSchedResult::DbSchedResultOK ||
        r.db_tip().height != primary.db_tip().height ||
        r.db_tip().id != primary.db_tip().id) {
      error("db shard tip divergence: shard {} at height {} (id {}, result {}), "
            "shard 0 at height {} (id {}); refusing to start",
            i, r.db_tip().height, r.db_tip().id, r.result(),
            primary.db_tip().height, primary.db_tip().id);
    }
    CHECK_EQ(r.result(), DbSchedResult::DbSchedResultOK);
    CHECK_EQ(r.db_tip().value.proposal_header.block_height,
             primary.db_tip().value.proposal_header.block_height);
    CHECK_EQ(r.db_tip().id, primary.db_tip().id);
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
