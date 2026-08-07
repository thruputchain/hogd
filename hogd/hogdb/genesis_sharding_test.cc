#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
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
#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId, shard_of_address
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h" // BlockTXID
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {
namespace {

struct GenesisHashes {
  StorageLongHash partial;        // committed DbTip.storage_long_hash
  StorageShortHash global_header; // header storage_hash == sha256(global)
  // The genesis hog supply, if this shard kept the row at all. Hog ages are not
  // state items, so they never show up in the hashes above.
  std::optional<int128_t> supply_hogs;
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
  std::optional<int128_t> supply_hogs;
  if (HogDBStatus<HogAge> age = ctx.get_db().read_address_average_hog_age(
          GenesisConfigurationLoader::kDeployContractAddress);
      age.has_value()) {
    supply_hogs = from_xdr(age->num_hogs[kHogAgeGroupToSchedule]);
  }
  return {.partial = tip.storage_long_hash,
          .global_header = tip.value.proposal_header.storage_hash,
          .supply_hogs = supply_hogs};
}

// Each shard writes only the items it owns, so their partial hashes must sum to
// the single-shard hash -- which is also the global hash every shard's header
// carries. The only coverage of the genesis filtering for N > 1.
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

// The genesis hog supply is keyed by kDeployContractAddress and must live on
// exactly the shard that owns that address: hog ages are 'A' keys, routed by
// address like everything else, but excluded from the db hash -- so
// PartialsSumToSingleShardGlobalForAnyShardCount above cannot see them, and a
// supply row written on the wrong shard, on every shard, or on none would leave
// both tests above passing.
//
// Getting this wrong is not subtle in production: the scheduler reads the
// supply through the owning shard, finds nothing, and discards the first batch
// of transactions for having no hogs.
// Asserts placement only. The row's kHogAgeGroupToSchedule reads back as 0
// rather than initial_hog_token_supply(), at every shard count including 1, so
// that is not a sharding property and is deliberately not asserted here -- the
// scheduler learns the supply from the block-0 transfer deltas instead. Worth
// separating out before anyone relies on the value at rest.
TEST(GenesisSharding, SupplyHogAgeLandsOnItsOwningShardOnly)
{
  for (uint32_t n : {1U, 2U, 3U, 4U}) {
    const size_t owner =
        shard_of_address(GenesisConfigurationLoader::kDeployContractAddress, n);
    for (uint32_t s = 0; s < n; ++s) {
      GenesisHashes shard = load_genesis_for_shard(s, n);
      if (s == owner) {
        EXPECT_TRUE(shard.supply_hogs.has_value())
            << "shard " << s << " of " << n
            << " owns the supply but has no row";
      }
      else {
        EXPECT_FALSE(shard.supply_hogs.has_value())
            << "shard " << s << " of " << n
            << " does not own the supply but kept a row";
      }
    }
  }
}

} // namespace
} // namespace hogd
