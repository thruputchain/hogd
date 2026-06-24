#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "parallel.hh"
#include "transaction.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

TEST(Int128XdrBytesTest, MatchesXdrSerializeExactly)
{
  constexpr int128_t kValue =
      (int128_t{0x0102030405060708LL} << 64) | 0x1112131415161718ULL;

  auto encoded = int128_to_xdr_bytes(kValue);
  auto serialized = x23::serialize(to_xdr(kValue));
  std::span<const std::byte> serialized_bytes =
      std::as_bytes(std::span(serialized));

  ASSERT_EQ(encoded.size(), serialized_bytes.size());
  EXPECT_TRUE(
      std::equal(encoded.begin(), encoded.end(), serialized_bytes.begin()));
}

TEST(Int128XdrBytesTest, RoundTripsPositiveAndNegativeValues)
{
  constexpr int128_t kPositive =
      (int128_t{0x1122334455667788LL} << 64) | 0x99AABBCCDDEEFF00ULL;
  constexpr int128_t kNegative =
      -((int128_t{0x0123456789ABCDEFLL} << 64) | 0x0011223344556677ULL);

  EXPECT_EQ(int128_from_xdr_bytes(int128_to_xdr_bytes(kPositive)), kPositive);
  EXPECT_EQ(int128_from_xdr_bytes(int128_to_xdr_bytes(kNegative)), kNegative);
}

namespace {

SignedTransaction
make_minimal_signed_transaction()
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0;
  claim.source_address_claim().required_weight = 0;

  return SignedTransaction{
      .tx =
          Transaction{
              .invocation =
                  Invocation{
                      .invoked_address = {},
                      .entrypoint = {},
                      .gas_limit = 1,
                      .calldata = {},
                  },
              .uniquifier = {.block_expiry_height = 1, .nonce = 0},
              .contracts_to_deploy = {},
              .auth_claim = std::move(claim),
          },
      .signatures = {},
  };
}

} // namespace

TEST(ParallelBatchToTTest, RoundTripsSingleSignedTransaction)
{
  const SignedTransaction tx = make_minimal_signed_transaction();
  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{tx})();

  SignedTransactionBatch parsed;
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed)());
  ASSERT_EQ(parsed.size(), 1UL);
  EXPECT_EQ(parsed[0], tx);
}

TEST(ParallelBatchToTTest, AppendToEmptyMatchesTToParallelBatch)
{
  const SignedTransaction tx = make_minimal_signed_transaction();
  SignedTransactionBatch txs{tx};
  ParallelSignedTransactionBatch appended;
  append_to_parallel_batch(txs, appended)();
  ParallelSignedTransactionBatch replaced = t_to_parallel_batch(txs)();

  EXPECT_EQ(appended, replaced);
}

TEST(ParallelBatchToTTest, AppendPreservesExistingDataAndOrder)
{
  SignedTransaction tx0 = make_minimal_signed_transaction();
  SignedTransaction tx1 = make_minimal_signed_transaction();
  tx1.tx.uniquifier.nonce = 1;

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{tx0})();
  const size_t initial_num_segments = batch.segment_tokens.size();
  const size_t initial_num_bytes = batch.xdr.size();

  append_to_parallel_batch(SignedTransactionBatch{tx1}, batch)();

  EXPECT_GT(batch.segment_tokens.size(), initial_num_segments);
  EXPECT_GT(batch.xdr.size(), initial_num_bytes);

  SignedTransactionBatch parsed;
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed)());
  ASSERT_EQ(parsed.size(), 2UL);
  EXPECT_EQ(parsed[0], tx0);
  EXPECT_EQ(parsed[1], tx1);
}

TEST(ParallelBatchToTTest, RejectsOverclaimedFinalSegment)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  ASSERT_EQ(batch.segment_tokens[0], 1U);
  ASSERT_EQ(batch.segment_bytes[0], batch.xdr.size());
  batch.segment_tokens[0] = 2;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RejectsLeftoverSegmentBytes)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  ASSERT_EQ(batch.segment_tokens[0], 1U);
  batch.xdr.resize(batch.xdr.size() + 4, 0);
  batch.segment_bytes[0] += 4;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RejectsXdrLargerThanSegmentBytes)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.xdr.resize(batch.xdr.size() + 4, 0);

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RejectsSegmentBytesLargerThanXdr)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_bytes[0] += 4;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RandomByteCorruptionDoesNotCrash)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> idx_dist(0, batch.xdr.size() - 1);
  std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

  for (int trial = 0; trial < 100; trial++) {
    ParallelSignedTransactionBatch corrupted = batch;
    const size_t num_mutations =
        std::uniform_int_distribution<size_t>(1, batch.xdr.size())(rng);
    for (size_t m = 0; m < num_mutations; m++) {
      corrupted.xdr[idx_dist(rng)] = byte_dist(rng);
    }
    SignedTransactionBatch parsed;
    (void)parallel_batch_to_t(corrupted, parsed)();
  }
}

// A small batch must not be able to force a huge [objs] allocation by claiming
// far more segment tokens than the payload bytes could possibly encode. The two
// maxed-out token counts sum to more than the decoded vector's max_size(), so
// the pre-fix unbounded [objs.resize(total_segment_tokens)] threw an
// XdrLengthError instead of rejecting cleanly; the empty payload also means no
// token could legitimately be decoded.
// Regression test for groundhogchain/hogd#1645.
TEST(ParallelBatchToTTest, RejectsTokenCountExceedingPayloadBytes)
{
  ParallelBatch batch{
      .segment_tokens = {0xffffffff, 0xffffffff},
      .segment_bytes = {0, 0},
      .xdr = {},
  };

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RejectsBoundedVectorLengthError)
{
  using BoundedUint32Vec = x23::Vec<std::uint32_t, 1>;

  ParallelBatch batch{
      .segment_tokens = {1},
      .segment_bytes = {12},
      .xdr = {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2},
  };

  x23::XdrVec<BoundedUint32Vec> parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

} // namespace hogd
