#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "consensus.hh" // ConsensusValue
#include "db_sched.hh"  // DbSchedResult
#include "genesis.hh"   // GenesisConfiguration
#include "rpc_types.hh" // DbTip
#include "sched_db.hh"  // ShardIdentity
#include "types.hh"

#include "hogd/base/io.h" // Directory
#include "hogd/base/log.h"
#include "hogd/base/status.h" // ErrnoStatus
#include "hogd/base/types.h"  // 10_MiB
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h" // BlockTXID
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {
namespace {

struct GenesisHashes {
  StorageLongHash partial;        // committed DbTip.storage_long_hash
  StorageShortHash global_header; // header storage_hash == sha256(global)
};

// Load genesis on a fresh, empty db under the given shard identity and return
// this shard's committed PARTIAL hash and the block header's GLOBAL hash.
GenesisHashes
load_genesis_for_shard(uint32_t shard_id, uint32_t total_shards)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(static_cast<bool>(dir));
  constexpr size_t kHeapSize = 10_MiB;
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                     0);
  CHECK(db.has_value());
  HogDBDAO dao(RAMDB(std::move(*db)));
  RPCContext ctx(std::move(dao), nullptr);

  // Fresh db: tip at height -1 so genesis is allowed to load.
  ConsensusValue tip_value{};
  tip_value.proposal_header.block_height = -1;
  ctx.set_tip(DbTip{.id = std::numeric_limits<BlockTXID>::max(),
                    .storage_long_hash = {},
                    .value = tip_value});
  ctx.set_shard_identity(ShardIdentity{.shard_id = shard_id,
                                       .total_shards = total_shards,
                                       .shard_function_id = kShardFunctionId});

  HogDBSchedRpc sched(ctx);
  ConsensusValue genesis_value{};
  genesis_value.proposal_header.block_height = 0;
  DbSchedResult res =
      sched.dbsched_load_genesis_configuration(GenesisConfiguration{},
                                               genesis_value)();
  CHECK_EQ(res, DbSchedResult::DbSchedResultOK);

  DbTip tip = ctx.get_tip();
  return {.partial = tip.storage_long_hash,
          .global_header = tip.value.proposal_header.storage_hash};
}

// Each shard writes only the genesis items it owns, so their partial hashes
// (LtHash16, additive) must sum to the value a single-shard db computes --
// which is also the GLOBAL hash every shard puts in its block header. This is
// the only coverage of the shard-owner-only genesis filtering for N > 1; the
// rest of the suite runs single-shard.
TEST(GenesisSharding, PartialsSumToSingleShardGlobalForAnyShardCount)
{
  GenesisHashes single = load_genesis_for_shard(/*shard_id=*/0,
                                                /*total_shards=*/1);
  // Genesis writes real state items, so the single-shard hash is non-empty.
  EXPECT_NE(single.partial, StorageLongHash{});
  // At N=1 everything is owned, so the header (global) == sha256(partial).
  EXPECT_EQ(single.global_header,
            sha256(std::as_bytes(std::span(single.partial))));

  for (uint32_t n : {2U, 3U, 4U}) {
    LtHash16 sum;
    for (uint32_t s = 0; s < n; ++s) {
      GenesisHashes shard = load_genesis_for_shard(s, n);
      sum += lthash16_from_db_hash(shard.partial);
      // Every shard commits the same global hash into the block header, so the
      // block hash is identical regardless of N.
      EXPECT_EQ(shard.global_header, single.global_header)
          << "shard " << s << " of " << n;
    }
    // The shards' partials sum to the single-shard (global) hash.
    EXPECT_EQ(db_hash_from_lthash16(sum), single.partial) << "n=" << n;
  }
}

} // namespace
} // namespace hogd
