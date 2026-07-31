#include "hogd/mempool/sharded_db_sched.h"

#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "header.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h" // kBulkRpcMaxMessageSize
#include "hogd/base/rpc_server.h" // create_client, server_address_to_sockaddr
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"             // sha256
#include "hogd/crypto/lthash16.h"           // LtHash16
#include "hogd/db_shard_routing/sharding.h" // shard_of_address, kShardFunctionId
#include "hogd/hogdb/db_hash.h" // db_hash_from_lthash16, lthash16_from_db_hash
#include "hogd/mempool/shared.h"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// Each shard hashes only the items it owns, so its storage_long_hash is a
// partial LtHash16 multiset value and the partials sum to the whole (see
// sharding.h). Summing here -- rather than putting a per-shard vector in the
// header -- keeps the header identical to the single-shard case and lets
// validators run different shard counts.
//
// First non-OK in shard order wins: it carries no hash, and no caller reads a
// hash off the non-OK path.
DbBlockResult
combine_block_results(std::vector<DbBlockResult> const &results)
{
  CHECK(!results.empty()); // one entry per shard; shard 0 is the base below
  LtHash16 total;
  for (DbBlockResult const &r : results) {
    if (r.result() != DbSchedResult::DbSchedResultOK) {
      return r;
    }
    total += lthash16_from_db_hash(r.block().storage_long_hash);
  }

  DbBlockResult combined = results[kPrimaryShard];
  combined.block().storage_long_hash = db_hash_from_lthash16(total);
  combined.block().storage_hash =
      sha256(std::as_bytes(std::span(combined.block().storage_long_hash)));
  return combined;
}

x23::Async<std::shared_ptr<ShardedDbSched>>
ShardedDbSched::connect(x23::XdrVec<ShardCard> cards)
{
  size_t const n = cards.size();
  CHECK_GT(n, size_t{0});

  // Bind clients by declared shard_id (fail-stop on a bad set).
  auto order_result = order_shards_by_id(cards);
  if (!order_result) {
    panic("ShardedDbSched::connect: {}; refusing to start",
          order_result.error());
  }
  std::vector<size_t> const &order = *order_result;

  std::vector<std::shared_ptr<db_sched_client_t>> clients(n);
  for (size_t id = 0; id < n; ++id) {
    ShardCard const &card = cards[order[id]];
    auto client = co_await create_client<DbSched1>(
        server_address_to_sockaddr(card.address_for_sched),
        /*retry=*/false,
        /*max_message_size=*/kBulkRpcMaxMessageSize);
    if (!client) {
      panic("ShardedDbSched::connect failed to connect db shard {}: {}", id,
            client.error().error_code().message());
    }
    clients[id] = std::make_shared<db_sched_client_t>(std::move(*client));
  }
  co_return std::make_shared<ShardedDbSched>(std::move(clients));
}

x23::Async<DbSchedResult>
ShardedDbSched::create_snapshot(BlockID prev_block_id, BlockID block_id,
                                BlockExecutionMode mode)
{
  // All-or-nothing: create must succeed on every shard. Collect per-shard
  // results (not a reduced first-error) so that on a partial failure we can
  // abandon exactly the shards that did create -- otherwise those shards leak
  // an open BlockTX for this block.
  std::vector<DbSchedResult> results =
      co_await shards_.broadcast_collect<DbSchedResult>([&](auto &client) {
        return client->dbsched_create_snapshot(prev_block_id, block_id, mode);
      });

  DbSchedResult first_error = DbSchedResult::DbSchedResultOK;
  for (DbSchedResult r : results) {
    if (r != DbSchedResult::DbSchedResultOK) {
      first_error = r;
      break;
    }
  }
  if (first_error == DbSchedResult::DbSchedResultOK) {
    co_return DbSchedResult::DbSchedResultOK;
  }

  // Roll back the shards that succeeded. Abandon only those: abandoning a shard
  // that never created returns non-OK and trips a CHECK downstream. Best-effort
  // -- log but keep the original create error, which is what we report.
  for (size_t s = 0; s < shards_.shard_count(); ++s) {
    if (results[s] != DbSchedResult::DbSchedResultOK) {
      continue;
    }
    DbSchedResult abandoned =
        co_await shards_.shard(s)->dbsched_abandon_snapshot(block_id);
    if (abandoned != DbSchedResult::DbSchedResultOK) {
      error("create_snapshot rollback: abandon_snapshot failed on shard {} for "
            "block {}: {}",
            s, block_id, abandoned);
    }
  }
  co_return first_error;
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
  // Visit every shard (a close that only some shards saw would leave the rest
  // holding an open BlockTX), then reduce to the combined hash.
  std::vector<DbBlockResult> results =
      co_await shards_.broadcast_collect<DbBlockResult>([&](auto &client) {
        return client->dbsched_close_snapshot(block_id);
      });
  co_return combine_block_results(results);
}

x23::Async<DbBlockResult>
ShardedDbSched::commit_snapshot(BlockID block_id, ConsensusValue value)
{
  std::vector<DbBlockResult> results =
      co_await shards_.broadcast_collect<DbBlockResult>([&](auto &client) {
        return client->dbsched_commit_snapshot(block_id, value);
      });
  co_return combine_block_results(results);
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
  // Startup fail-stop for a partially-committed cluster: read every shard and
  // require (1) unanimous genesis presence (else a wiped shard 0 re-runs
  // genesis against shards that have it) and (2) agreement on the whole
  // replicated header -- height, id, txs_hash, and storage_hash. storage_hash
  // is the *global* (summed) db hash on every shard, for genesis as well as for
  // normal blocks, so it agrees; only DbTip.storage_long_hash is per-shard.
  // Checking it here is what makes a shard that committed against a different
  // item set fail at startup. Log before each CHECK.
  std::vector<DbTipResult> tips =
      co_await shards_.broadcast_collect<DbTipResult>(
          [](auto &client) { return client->dbsched_get_tip(); });

  DbTipResult const &primary = tips[kPrimaryShard];
  bool const primary_ok = primary.result() == DbSchedResult::DbSchedResultOK;
  for (size_t i = 1; i < tips.size(); ++i) {
    DbTipResult const &r = tips[i];
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
        r_header.txs_hash != primary_header.txs_hash ||
        r_header.storage_hash != primary_header.storage_hash) {
      error(
          "db shard tip divergence: shard {} at height {} (id {}), shard 0 at "
          "height {} (id {}); refusing to start",
          i, r_header.block_height, r.db_tip().id, primary_header.block_height,
          primary.db_tip().id);
    }
    CHECK_EQ(r_header.block_height, primary_header.block_height);
    CHECK_EQ(r.db_tip().id, primary.db_tip().id);
    CHECK_EQ(r_header.txs_hash, primary_header.txs_hash);
    CHECK_EQ(r_header.storage_hash, primary_header.storage_hash);
  }

  if (!primary_ok) {
    co_return primary;
  }

  // Hand back the combined db hash: each shard persists (and re-verifies on
  // restart) only its own partial, but everything above the facade wants the
  // whole-db value.
  DbTipResult combined = primary;
  LtHash16 total;
  for (DbTipResult const &r : tips) {
    total += lthash16_from_db_hash(r.db_tip().storage_long_hash);
  }
  combined.db_tip().storage_long_hash = db_hash_from_lthash16(total);
  co_return combined;
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
