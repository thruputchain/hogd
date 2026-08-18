#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "db_sched.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hog_db/db_hash.h"
#include "hogd/hog_mempool/sharded_db_sched.h"
#include "types.hh"

namespace hogd {
namespace {

// A shard's OK close/commit result carrying `partial` as its db hash.
DbBlockResult
ok_result(LtHash16 const &partial)
{
  DbBlockResult r(DbSchedResult::DbSchedResultOK);
  r.block().storage_long_hash = db_hash_from_lthash16(partial);
  r.block().storage_hash =
      sha256(std::as_bytes(std::span(r.block().storage_long_hash)));
  return r;
}

// A distinct non-empty LtHash16, standing in for one shard's owned items.
LtHash16
partial_for(std::uint8_t seed)
{
  LtHash16 h;
  std::array<std::uint8_t, 1> element{seed};
  h.add(x23::BufView(std::as_bytes(std::span(element))));
  return h;
}

// The whole point of the reduction: a block's header hash must not depend on
// how many shards the validator runs. Splitting the same item set across N
// shards must produce the hash a single shard would have returned.
TEST(ShardedDbSched, CombinesPartialHashesIntoTheSingleShardHash)
{
  LtHash16 a = partial_for(1);
  LtHash16 b = partial_for(2);
  LtHash16 c = partial_for(3);

  DbBlockResult unsharded = combine_block_results({ok_result(a + b + c)});
  DbBlockResult sharded =
      combine_block_results({ok_result(a), ok_result(b), ok_result(c)});

  ASSERT_EQ(sharded.result(), DbSchedResult::DbSchedResultOK);
  EXPECT_EQ(sharded.block().storage_long_hash,
            unsharded.block().storage_long_hash);
  EXPECT_EQ(sharded.block().storage_hash, unsharded.block().storage_hash);
  // The short hash the header carries is sha256 of the summed long hash.
  EXPECT_EQ(sharded.block().storage_hash,
            sha256(
                std::as_bytes(std::span(sharded.block().storage_long_hash))));
}

// Order matters: the sum must not depend on which shard reports first.
TEST(ShardedDbSched, CombinedHashIsIndependentOfShardOrder)
{
  LtHash16 a = partial_for(7);
  LtHash16 b = partial_for(9);

  EXPECT_EQ(combine_block_results({ok_result(a), ok_result(b)})
                .block()
                .storage_long_hash,
            combine_block_results({ok_result(b), ok_result(a)})
                .block()
                .storage_long_hash);
}

// A shard that failed contributes no hash, so the reduction must surface the
// failure rather than hand back a hash summed over the shards that did answer.
TEST(ShardedDbSched, SurfacesFirstNonOkResultFromAnyShard)
{
  DbBlockResult err(DbSchedResult::DbSchedResultError);

  EXPECT_EQ(combine_block_results({err, ok_result(partial_for(1))}).result(),
            DbSchedResult::DbSchedResultError);
  // Including when it is a later shard that failed and shard 0 succeeded.
  EXPECT_EQ(combine_block_results({ok_result(partial_for(1)), err}).result(),
            DbSchedResult::DbSchedResultError);
}

} // namespace
} // namespace hogd
