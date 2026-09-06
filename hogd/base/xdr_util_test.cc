#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <type_traits>
#include <utility>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "parallel.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

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

// #3131: int128_to_xdr_bytes / int128_from_xdr_bytes reach their std::byte row
// storage through uint8_t (via x23::putbe/getbe). That object-representation
// access is only alias-permitted when uint8_t is a character type; the helpers
// now pin that precondition at compile time. Mirror the guard here and exercise
// a decode from an independently-populated std::byte buffer read back through a
// span -- the shape of the persisted contract-data read path.
TEST(Int128XdrBytesTest, DecodesFromStandaloneByteBufferAliasSafely)
{
  static_assert(std::is_same_v<std::uint8_t, unsigned char>,
                "NNInt128 XDR byte helpers depend on uint8_t being "
                "unsigned char for alias-safe object-representation access");

  constexpr int128_t kValue =
      (int128_t{0x0102030405060708LL} << 64) | 0x1112131415161718ULL;

  // Big-endian 16-byte encoding of kValue, staged into a fresh std::byte
  // buffer without going through the helper's own array.
  std::array<std::byte, sizeof(int128_xdr)> buffer{};
  for (size_t i = 0; i < buffer.size(); i++) {
    buffer[i] = std::byte{static_cast<unsigned char>(
        static_cast<uint128_t>(kValue) >> (8 * (buffer.size() - i - 1)))};
  }

  EXPECT_EQ(int128_from_xdr_bytes(
                std::span<const std::byte, sizeof(int128_xdr)>(buffer)),
            kValue);

  // And the helper's own encode must land in that same canonical layout.
  auto encoded = int128_to_xdr_bytes(kValue);
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), buffer.begin()));
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
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
  ASSERT_EQ(parsed.size(), 1UL);
  EXPECT_EQ(parsed[0], tx);
}

TEST(ParallelBatchToTTest, EncodingMatchesIndividualSerializationAcrossSegments)
{
  for (size_t count : {0UL, 1UL, 19UL, 20UL, 21UL, 131UL}) {
    SignedTransactionBatch txs;
    for (size_t i = 0; i < count; ++i) {
      auto tx = make_minimal_signed_transaction();
      tx.tx.invocation.calldata.resize(i % 37, static_cast<uint8_t>(i));
      txs.push_back(std::move(tx));
    }
    ParallelBatch expected;
    ParallelBatch actual;
    // Repeated appends must also preserve the existing bytes and metadata.
    for (size_t repeat = 0; repeat < 2; ++repeat) {
      for (const auto &chunk :
           make_chunks(count, kNumThreadsForParallelBatch)) {
        const size_t start = expected.xdr.size();
        for (size_t j = chunk.start; j < chunk.end; ++j) {
          expected.xdr += x23::serialize(txs[j]);
        }
        expected.segment_tokens.push_back(chunk.end - chunk.start);
        expected.segment_bytes.push_back(expected.xdr.size() - start);
      }
      append_to_parallel_batch(txs, actual)();
      EXPECT_EQ(x23::serialize(actual), x23::serialize(expected));
    }
  }
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

  EXPECT_EQ(batch.segment_tokens.size(), initial_num_segments + 1);
  EXPECT_GT(batch.xdr.size(), initial_num_bytes);

  SignedTransactionBatch parsed;
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
  ASSERT_EQ(parsed.size(), 2UL);
  EXPECT_EQ(parsed[0], tx0);
  EXPECT_EQ(parsed[1], tx1);
}

TEST(ParallelBatchToTTest, AcceptsABatchAtTheObjectLimit)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction(),
                             make_minimal_signed_transaction()})();

  SignedTransactionBatch parsed;
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed, 2)());
  EXPECT_EQ(parsed.size(), 2UL);
}

TEST(ParallelBatchToTTest, RejectsABatchOverTheObjectLimit)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction(),
                             make_minimal_signed_transaction()})();

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, 1)());
  // Nothing was allocated for the objects the batch claimed.
  EXPECT_TRUE(parsed.empty());
}

// The limit applies to what the metadata claims, so the payload never has to
// be large enough to back it.
TEST(ParallelBatchToTTest, RejectsAnOverclaimAgainstTheObjectLimit)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0] = 0xffffffff;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, 1)());
  EXPECT_TRUE(parsed.empty());
}

TEST(ParallelBatchToTTest, AZeroObjectLimitAcceptsOnlyAnEmptyBatch)
{
  ParallelSignedTransactionBatch empty =
      t_to_parallel_batch(SignedTransactionBatch{})();
  SignedTransactionBatch parsed;
  EXPECT_TRUE(parallel_batch_to_t(empty, parsed, 0)());
  EXPECT_TRUE(parsed.empty());

  ParallelSignedTransactionBatch one = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();
  EXPECT_FALSE(parallel_batch_to_t(one, parsed, 0)());
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
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
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
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
}

TEST(ParallelBatchToTTest, RejectsXdrLargerThanSegmentBytes)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.xdr.resize(batch.xdr.size() + 4, 0);

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
}

TEST(ParallelBatchToTTest, RejectsSegmentBytesLargerThanXdr)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_bytes[0] += 4;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
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
    (void)parallel_batch_to_t(corrupted, parsed, kNoBatchSizeLimit)();
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
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
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
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed, kNoBatchSizeLimit)());
}

namespace {

SignedTransaction
make_shared_ptr_test_tx(std::uint64_t nonce, size_t calldata_bytes)
{
  SignedTransaction tx{};
  tx.tx.uniquifier.nonce = nonce;
  tx.tx.invocation.gas_limit = 1234;
  tx.tx.invocation.calldata.resize(calldata_bytes);
  for (size_t i = 0; i < calldata_bytes; i++) {
    tx.tx.invocation.calldata[i] = static_cast<std::uint8_t>(i + nonce);
  }
  return tx;
}

} // namespace

// [XdrSharedPtr] exists to change who owns the bytes, not what the bytes are,
// so pin the encoding against [x23::Ptr] rather than trusting the type system
// to have meant it.
TEST(XdrSharedPtrTest, SerializesByteForByteLikeXdrPtr)
{
  const SignedTransaction tx =
      make_shared_ptr_test_tx(/*nonce=*/7, /*calldata_bytes=*/37);

  const x23::Ptr<SignedTransaction> owned(tx);
  const XdrSharedPtr<SignedTransaction> shared(
      std::make_shared<const SignedTransaction>(tx));

  EXPECT_EQ(xdr_size(shared), x23::xdr_size(owned));
  EXPECT_EQ(x23::serialize(shared), x23::serialize(owned));
}

TEST(XdrSharedPtrTest, NullSerializesLikeNullXdrPtr)
{
  const x23::Ptr<SignedTransaction> owned(nullptr);
  const XdrSharedPtr<SignedTransaction> shared;

  EXPECT_EQ(xdr_size(shared), 4UL);
  EXPECT_EQ(xdr_size(shared), x23::xdr_size(owned));
  EXPECT_EQ(x23::serialize(shared), x23::serialize(owned));
}

TEST(XdrSharedPtrTest, VectorMatchesVectorOfXdrPtrs)
{
  const SignedTransaction first =
      make_shared_ptr_test_tx(/*nonce=*/1, /*calldata_bytes=*/0);
  const SignedTransaction second =
      make_shared_ptr_test_tx(/*nonce=*/2, /*calldata_bytes=*/64);

  // Present/absent/present, the shape a lookup reply actually produces.
  x23::XdrVec<x23::Ptr<SignedTransaction>> owned;
  owned.push_back(x23::Ptr<SignedTransaction>(first));
  owned.push_back(nullptr);
  owned.push_back(x23::Ptr<SignedTransaction>(second));

  x23::XdrVec<XdrSharedPtr<SignedTransaction>> shared;
  shared.push_back(XdrSharedPtr<SignedTransaction>(
      std::make_shared<const SignedTransaction>(first)));
  shared.push_back({});
  shared.push_back(XdrSharedPtr<SignedTransaction>(
      std::make_shared<const SignedTransaction>(second)));

  EXPECT_EQ(x23::xdr_size(shared), x23::xdr_size(owned));
  EXPECT_EQ(x23::serialize(shared), x23::serialize(owned));
}

// A vector of these must be usable where the wire type is a vector of
// optionals -- that is what lets an RPC handler return one.
static_assert(
    x23::serializes_like<x23::XdrVec<XdrSharedPtr<SignedTransaction>>,
                         x23::Vec<x23::Ptr<SignedTransaction>, 0xffffffff>>);

TEST(XdrSharedPtrTest, SharesTheOriginalObjectRatherThanCopyingIt)
{
  const std::shared_ptr<const SignedTransaction> tx =
      std::make_shared<const SignedTransaction>(
          make_shared_ptr_test_tx(/*nonce=*/3, /*calldata_bytes=*/16));

  const XdrSharedPtr<SignedTransaction> shared(tx);

  EXPECT_EQ(shared.get(), tx.get());
  EXPECT_EQ(tx.use_count(), 2L);
}

} // namespace hogd
