#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <latch>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/block_transfers.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/rpc_from_gateway.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;
constexpr size_t kNumTxs = 100;

TransactionHashBatch
hash_txs(const SignedTransactionBatch &txs)
{
  TransactionHashBatch hashes;
  hashes.reserve(txs.size());
  for (const SignedTransaction &tx : txs) {
    hashes.push_back(sha256(tx.tx));
  }
  return hashes;
}

// A block's transfers as they arrive when hog_db sent them as one chunk.
BlockTransfers
one_chunk_transfer(BlockHeight block, DeltaChunk deltas)
{
  std::vector<DeltaChunk> chunks;
  chunks.push_back(std::move(deltas));
  return BlockTransfers{.block = block,
                        .deltas = ChunkedVector<DeltaChunk>(std::move(chunks))};
}

} // namespace

TEST(Scheduler, ReadAge)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->read_average_hog_ages({MempoolTestInfra::kAccountAddresses[0]})();
  const auto [ret, lock] =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  EXPECT_EQ(ret->num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret->average_ages[kHogAgeGroupToSchedule], 0);
}

TEST(Scheduler, AddVettedTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  const SchedGatewaySubmitOutcomeBatch outcomes =
      scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(Scheduler, AddVettedTxFromUnknownAccount)
{
  const ContractAddress kUnknownAccount = make_account_address(99);

  fib_client_t fib_client(kUnknownAccount);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  const SchedGatewaySubmitOutcomeBatch outcomes =
      scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();

  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitInternalError);
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[0].tx)), nullptr);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, IsPendingReturnsFalseForUncachedAccount)
{
  const ContractAddress kUncachedAccount = make_account_address(99);
  fib_client_t fib_client(kUncachedAccount);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_FALSE(scheduler->is_pending(txs[0], sha256(txs[0].tx)));
}

TEST(Scheduler, BatchAdmissionPreservesMixedOutcomesAndDuplicates)
{
  // Exercise empty, small, uneven and fully parallel batches. Unknown
  // accounts and old failure outcomes must retain their original indices.
  for (const size_t count :
       {0UL, 1UL, 255UL, 256UL, 257UL, 1023UL, 1024UL, 1025UL, 8193UL}) {
    SCOPED_TRACE(count);
    MempoolTestInfra server;
    auto scheduler = server.mempool_coord_rpc().scheduler();
    server.mempool_coord_rpc().wait_for_block_commit(1)();
    fib_client_t client(MempoolTestInfra::kAccountAddresses[0]);
    SignedTransactionBatch txs = generate_unsigned_txs(client, count);
    size_t expected_pending = 0;
    for (size_t i = 0; i < count; ++i) {
      if (i % 257 == 1) {
        txs[i].tx.invocation.invoked_address = make_account_address(99);
      }
      else {
        ++expected_pending;
      }
    }
    const auto hashes = hash_txs(txs);
    for (const auto &hash : hashes) {
      scheduler->record_transaction_outcome(
          hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
          scheduler->current_block())();
    }
    for (size_t attempt = 0; attempt < 2; ++attempt) {
      const auto outcomes = scheduler->add_txs(txs, hashes)();
      ASSERT_EQ(outcomes.size(), count);
      EXPECT_EQ(scheduler->size(), static_cast<ssize_t>(expected_pending));
      EXPECT_EQ(scheduler->tx_cache_size(), expected_pending);
      for (size_t i = 0; i < count; ++i) {
        SCOPED_TRACE(i);
        const bool known = i % 257 != 1;
        EXPECT_EQ(
            outcomes[i],
            known ? SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted
                  : SchedGatewaySubmitOutcome::SchedGatewaySubmitInternalError);
        EXPECT_EQ(static_cast<bool>(scheduler->find_tx(hashes[i])), known);
        EXPECT_EQ(scheduler->lookup_transaction_outcome(hashes[i])().status,
                  known
                      ? SchedulerTransactionStatus::SchedulerTransactionPending
                      : SchedulerTransactionStatus::SchedulerTransactionRetry);
      }
    }
  }
}

TEST(Scheduler, LocalSchedulingDropsTxWhenAllHogAgeGroupsAreEmpty)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  const TransactionHash tx_hash = sha256(txs[0].tx);
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  auto lookup = gateway.schedgateway_lookup_tx({tx_hash})();
  ASSERT_EQ(lookup.size(), 1UL);
  EXPECT_TRUE(lookup[0]);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();

  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
  EXPECT_FALSE(scheduler->find_tx(tx_hash));

  lookup = gateway.schedgateway_lookup_tx({tx_hash})();
  ASSERT_EQ(lookup.size(), 1UL);
  EXPECT_FALSE(lookup[0]);

  // A later tx submission must restore the tx to both the [Scheduler::txs_]
  // cache and the runqueue ([Scheduler::accounts_]).
  const SchedGatewaySubmitOutcomeBatch outcomes =
      scheduler->add_txs({txs[0]}, {tx_hash})();
  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);
  EXPECT_TRUE(scheduler->find_tx(tx_hash));
}

TEST(Scheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  const SchedGatewaySubmitOutcomeBatch first =
      scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  ASSERT_EQ(first.size(), 1UL);
  EXPECT_EQ(first[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(scheduler->size(), 1UL);
  // Now add the transaction a second time.
  const SchedGatewaySubmitOutcomeBatch duplicate =
      scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  ASSERT_EQ(duplicate.size(), 1UL);
  EXPECT_EQ(duplicate[0],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(Scheduler, LatestFailedTransactionOutcomeWins)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransaction tx = generate_unsigned_txs(fib_client, 1)[0];
  const TransactionHash tx_hash = sha256(tx.tx);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->set_current_block(0);
  scheduler->record_transaction_outcome(
      tx_hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
      scheduler->current_block(),
      SchedulerTransactionErrorSource::SchedulerTransactionErrorScheduler,
      static_cast<int32_t>(
          SchedulerErrorCode::SchedulerErrorHogAgeUnavailable))();

  EXPECT_EQ(scheduler->lookup_transaction_outcome(tx_hash)().height, 0);

  const SchedGatewaySubmitOutcomeBatch outcomes =
      scheduler->add_txs({tx}, {tx_hash})();
  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(scheduler->lookup_transaction_outcome(tx_hash)().status,
            SchedulerTransactionStatus::SchedulerTransactionPending);

  EXPECT_EQ(scheduler->lookup_transaction_outcome(tx_hash)().height, 0);

  scheduler->set_current_block(43);
  const SchedulerTransactionOutcome pending =
      scheduler->lookup_transaction_outcome(tx_hash)();
  EXPECT_EQ(pending.status,
            SchedulerTransactionStatus::SchedulerTransactionPending);
  EXPECT_EQ(pending.height, 43);
  const TransactionHashBatch tx_hashes{tx_hash};
  scheduler->remove_from_scheduler(
      x23::span<const TransactionHash>(tx_hashes))();
  scheduler->record_transaction_outcome(
      tx_hash, SchedulerTransactionStatus::SchedulerTransactionDropped,
      scheduler->current_block(),
      SchedulerTransactionErrorSource::SchedulerTransactionErrorScheduler,
      static_cast<int32_t>(SchedulerErrorCode::SchedulerErrorExpired))();

  const SchedulerTransactionOutcome outcome =
      scheduler->lookup_transaction_outcome(tx_hash)();
  EXPECT_EQ(outcome.status,
            SchedulerTransactionStatus::SchedulerTransactionDropped);
  EXPECT_EQ(
      outcome.error_source,
      SchedulerTransactionErrorSource::SchedulerTransactionErrorScheduler);
  EXPECT_EQ(outcome.error_code,
            static_cast<int32_t>(SchedulerErrorCode::SchedulerErrorExpired));
  EXPECT_EQ(outcome.height, 43);
}

TEST(Scheduler, CommittedTransactionOutcomeIsNeverOverwritten)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransaction tx = generate_unsigned_txs(fib_client, 1)[0];
  const TransactionHash tx_hash = sha256(tx.tx);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->record_transaction_outcome(
      tx_hash, SchedulerTransactionStatus::SchedulerTransactionSucceeded,
      /*height=*/42,
      SchedulerTransactionErrorSource::SchedulerTransactionErrorNone, 0)();
  const SchedGatewaySubmitOutcomeBatch outcomes =
      scheduler->add_txs({tx}, {tx_hash})();
  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  scheduler->record_transaction_outcome(
      tx_hash, SchedulerTransactionStatus::SchedulerTransactionDropped,
      scheduler->current_block(),
      SchedulerTransactionErrorSource::SchedulerTransactionErrorScheduler,
      static_cast<int32_t>(SchedulerErrorCode::SchedulerErrorExpired))();

  const SchedulerTransactionOutcome outcome =
      scheduler->lookup_transaction_outcome(tx_hash)();
  EXPECT_EQ(outcome.status,
            SchedulerTransactionStatus::SchedulerTransactionSucceeded);
  EXPECT_EQ(outcome.error_source,
            SchedulerTransactionErrorSource::SchedulerTransactionErrorNone);
  EXPECT_EQ(outcome.height, 42);
}

TEST(Scheduler, BatchCommittedOutcomesPreserveSuccessAndReplaceFailure)
{
  Scheduler scheduler(/*current_block=*/10, nullptr);
  TransactionHash first{};
  TransactionHash second{};
  second[0] = 1;
  const TransactionHashBatch hashes{first, second, first};
  scheduler.record_transaction_outcome(
      first, SchedulerTransactionStatus::SchedulerTransactionSucceeded,
      /*height=*/7)();
  scheduler.record_transaction_outcome(
      second, SchedulerTransactionStatus::SchedulerTransactionRetry,
      /*height=*/9,
      SchedulerTransactionErrorSource::SchedulerTransactionErrorExecutor,
      /*error_code=*/123)();

  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/10)();
  const SchedulerTransactionOutcome first_outcome =
      scheduler.lookup_transaction_outcome(first)();
  EXPECT_EQ(first_outcome.status,
            SchedulerTransactionStatus::SchedulerTransactionSucceeded);
  EXPECT_EQ(first_outcome.height, 7);
  EXPECT_EQ(first_outcome.error_source,
            SchedulerTransactionErrorSource::SchedulerTransactionErrorNone);
  EXPECT_EQ(first_outcome.error_code, 0);
  const SchedulerTransactionOutcome second_outcome =
      scheduler.lookup_transaction_outcome(second)();
  EXPECT_EQ(second_outcome.status,
            SchedulerTransactionStatus::SchedulerTransactionSucceeded);
  EXPECT_EQ(second_outcome.height, 10);
  EXPECT_EQ(second_outcome.error_source,
            SchedulerTransactionErrorSource::SchedulerTransactionErrorNone);
  EXPECT_EQ(second_outcome.error_code, 0);
}

TEST(Scheduler, BatchCommittedOutcomesExpireFromProcessingHeight)
{
  Scheduler scheduler(/*current_block=*/10, nullptr);
  const TransactionHashBatch hashes{TransactionHash{}};
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/7)();
  scheduler.set_current_block(10 + Scheduler::kTransactionOutcomeBlockLifetime -
                              1);
  EXPECT_EQ(scheduler.lookup_transaction_outcome(hashes[0])().height, 7);
  scheduler.increment_current_block()();
  EXPECT_EQ(scheduler.lookup_transaction_outcome(hashes[0])().status,
            SchedulerTransactionStatus::SchedulerTransactionNotFound);
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/60)();
  EXPECT_EQ(scheduler.lookup_transaction_outcome(hashes[0])().height, 60);
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>{}, /*height=*/61)();
  EXPECT_EQ(scheduler.lookup_transaction_outcome(hashes[0])().height, 60);
}

TEST(Scheduler, BatchCommittedOutcomesRecordEveryTransaction)
{
  Scheduler scheduler(/*current_block=*/10, nullptr);
  TransactionHashBatch hashes;
  for (size_t i = 0; i < 5000; ++i) {
    TransactionHash hash{};
    hash[0] = static_cast<uint8_t>(i);
    hash[1] = static_cast<uint8_t>(i >> 8);
    hashes.push_back(hash);
  }
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/10)();
  for (const TransactionHash &hash : hashes) {
    const SchedulerTransactionOutcome outcome =
        scheduler.lookup_transaction_outcome(hash)();
    EXPECT_EQ(outcome.status,
              SchedulerTransactionStatus::SchedulerTransactionSucceeded);
    EXPECT_EQ(outcome.height, 10);
  }
}

TEST(Scheduler, BatchCommitWinsOverConcurrentRetryOutcomes)
{
  Scheduler scheduler(/*current_block=*/10, nullptr);
  TransactionHashBatch hashes;
  for (size_t i = 0; i < 8192; ++i) {
    TransactionHash hash{};
    hash[0] = static_cast<uint8_t>(i);
    hash[1] = static_cast<uint8_t>(i >> 8);
    hashes.push_back(hash);
  }
  std::jthread retries([&] {
    for (const auto &hash : hashes) {
      scheduler.record_transaction_outcome(
          hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
          /*height=*/9,
          SchedulerTransactionErrorSource::SchedulerTransactionErrorExecutor,
          /*error_code=*/123)();
    }
  });
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/10)();
  retries.join();
  for (const auto &hash : hashes) {
    const auto outcome = scheduler.lookup_transaction_outcome(hash)();
    EXPECT_EQ(outcome.status,
              SchedulerTransactionStatus::SchedulerTransactionSucceeded);
    EXPECT_EQ(outcome.height, 10);
    EXPECT_EQ(outcome.error_code, 0);
  }
}

TEST(Scheduler, BatchCommittedOutcomesCrossChunks)
{
  Scheduler scheduler(/*current_block=*/10, nullptr);
  constexpr size_t kChunkSize = Scheduler::kTransactionOutcomeChunkSize;
  TransactionHashBatch hashes;
  // The scheduler adds at most [kChunkSize] txs to a shard before it needs to
  // drop the shard lock and acquire it again. Thus, we create [(2 * kChunkSize)
  // + 1] hashes so that the scheduler needs to split the txs across 3 groups,
  // releasing/acquiring the shard lock each time.
  static_assert((2 * kChunkSize) + 1 < Scheduler::kMaxTransactionsPerShard);
  for (size_t nonce = 0; hashes.size() < (2 * kChunkSize) + 1; ++nonce) {
    TransactionHash hash{};
    std::memcpy(hash.data(), &nonce, sizeof(nonce));
    if (Scheduler::transaction_outcome_shard_index(hash) == 0) {
      hashes.push_back(hash);
    }
  }
  for (const auto &hash : hashes) {
    scheduler.record_transaction_outcome(
        hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
        /*height=*/9,
        SchedulerTransactionErrorSource::SchedulerTransactionErrorExecutor,
        /*error_code=*/123)();
  }

  const std::array<size_t, 5> probes{0, kChunkSize - 1, kChunkSize,
                                     (2 * kChunkSize) - 1, 2 * kChunkSize};
  std::latch reader_started(1);
  std::atomic<bool> commit_finished = false;
  std::jthread reader([&] {
    std::array<bool, probes.size()> committed{};
    const auto check_outcomes = [&] {
      for (size_t i = 0; i < probes.size(); ++i) {
        const auto outcome =
            scheduler.lookup_transaction_outcome(hashes[probes[i]])();
        if (outcome.status ==
            SchedulerTransactionStatus::SchedulerTransactionSucceeded) {
          committed[i] = true;
          EXPECT_EQ(outcome.height, 10);
          EXPECT_EQ(
              outcome.error_source,
              SchedulerTransactionErrorSource::SchedulerTransactionErrorNone);
          EXPECT_EQ(outcome.error_code, 0);
        }
        else {
          EXPECT_FALSE(committed[i]);
          EXPECT_EQ(outcome.status,
                    SchedulerTransactionStatus::SchedulerTransactionRetry);
          EXPECT_EQ(outcome.height, 9);
          EXPECT_EQ(outcome.error_source,
                    SchedulerTransactionErrorSource::
                        SchedulerTransactionErrorExecutor);
          EXPECT_EQ(outcome.error_code, 123);
        }
      }
    };
    check_outcomes();
    reader_started.count_down();
    while (!commit_finished.load(std::memory_order_acquire)) {
      check_outcomes();
    }
    check_outcomes();
    for (bool observed : committed) {
      EXPECT_TRUE(observed);
    }
  });
  reader_started.wait();
  scheduler.record_committed_transaction_outcomes(
      x23::span<const TransactionHash>(hashes), /*height=*/10)();
  commit_finished.store(true, std::memory_order_release);
  reader.join();

  for (const auto &hash : hashes) {
    const auto outcome = scheduler.lookup_transaction_outcome(hash)();
    EXPECT_EQ(outcome.status,
              SchedulerTransactionStatus::SchedulerTransactionSucceeded);
    EXPECT_EQ(outcome.height, 10);
    EXPECT_EQ(outcome.error_source,
              SchedulerTransactionErrorSource::SchedulerTransactionErrorNone);
    EXPECT_EQ(outcome.error_code, 0);
  }
}

TEST(Scheduler, ClearFailureAfterOutcomeReplacementAndExpiry)
{
  Scheduler scheduler(/*current_block=*/0, nullptr);
  const TransactionHash hash{};
  const auto record_failure = [&] {
    scheduler.record_transaction_outcome(
        hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
        scheduler.current_block(),
        SchedulerTransactionErrorSource::SchedulerTransactionErrorExecutor,
        /*error_code=*/123)();
  };
  const auto clear_failure = [&] {
    scheduler.clear_transaction_outcome_if_not_already_committed(hash)();
    EXPECT_EQ(scheduler.lookup_transaction_outcome(hash)().status,
              SchedulerTransactionStatus::SchedulerTransactionNotFound);
  };
  for (size_t i = 0; i < 2; ++i) {
    record_failure();
    clear_failure();
  }
  record_failure();
  scheduler.record_transaction_outcome(
      hash, SchedulerTransactionStatus::SchedulerTransactionSucceeded,
      /*height=*/0)();
  scheduler.clear_transaction_outcome_if_not_already_committed(hash)();
  EXPECT_EQ(scheduler.lookup_transaction_outcome(hash)().status,
            SchedulerTransactionStatus::SchedulerTransactionSucceeded);

  scheduler.set_current_block(Scheduler::kTransactionOutcomeBlockLifetime);
  scheduler.increment_current_block()();
  record_failure();
  clear_failure();
  record_failure();
  scheduler.set_current_block(2 * Scheduler::kTransactionOutcomeBlockLifetime);
  scheduler.increment_current_block()();
  record_failure();
  clear_failure();
}

TEST(Scheduler, CycleGroupsMovesHogsBetweenBuckets)
{
  HogAge hog_age{};
  hog_age.address = MempoolTestInfra::kAccountAddresses[0];
  hog_age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(1));
  hog_age.average_ages[kHogAgeGroupToSchedule] = 10;
  hog_age.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(static_cast<uint128_t>(2));
  hog_age.average_ages[kHogAgeGroupAlreadyScheduled] = 5;
  hog_age.num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(3));
  hog_age.average_ages[kHogAgeGroupReceived] = 1;

  Scheduler::cycle_groups(hog_age);

  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(3));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupToSchedule], 7);
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(3));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupAlreadyScheduled], 1);
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, Single)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  // Check that the correct number of hogs was deducted.
  auto [age, lock] =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  lock.unlock();

  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1);
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);

  // Check the correct number of hogs was deducted from the
  // [kHogAgeGroupToSchedule] group.
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0);

  // Check that the correct number of hogs was added to the
  // [kHogAgeGroupAlreadyScheduled] group.
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled],
            age->average_ages[kHogAgeGroupToSchedule] + 2);
}

TEST(Scheduler, PendingNominationKeepsLiveHogAgesUnchangedUntilCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();

  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  auto [before_commit, before_lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(before_commit->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(from_xdr(before_commit->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  before_lock.unlock();

  scheduler->start_pending_nomination(/*block_id=*/1);
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);

  auto [during_nomination, during_lock] =
      scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(during_nomination->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(from_xdr(during_nomination->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  during_lock.unlock();

  scheduler->commit_pending_nomination(/*block_id=*/1)();

  auto [after_commit, after_lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(after_commit->num_hogs[kHogAgeGroupToSchedule]),
            (5'000'000'000 * 10) - gas_limit);
  EXPECT_EQ(from_xdr(after_commit->num_hogs[kHogAgeGroupAlreadyScheduled]),
            gas_limit);
  EXPECT_EQ(after_commit->average_ages[kHogAgeGroupAlreadyScheduled], 2);
  after_lock.unlock();
}

TEST(Scheduler, PendingNominationExcludesUnschedulableAccountAcrossSchedules)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10;
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  txs[2].tx.uniquifier.block_expiry_height = 12;

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1], txs[2]},
                     hash_txs({txs[0], txs[1], txs[2]}))();
  EXPECT_EQ(scheduler->size(), 3UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduled_hashes.clear();
  scheduled.clear();
  // Check that no tx is scheduled.
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduler->commit_pending_nomination(/*block_id=*/1)();

  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[2]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, AddTxsDoesNotReenableAccountExcludedByPendingNomination)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10;
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  txs[2].tx.uniquifier.block_expiry_height = 12;

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 0UL);

  scheduler->add_txs({txs[2]}, hash_txs({txs[2]}))();
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduled_hashes.clear();
  scheduled.clear();
  // Check that no tx is scheduled.
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduler->abandon_pending_nomination(/*block_id=*/1)();

  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[2]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, CommitTouchingExcludedAccountRemovesFromScheduler)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto balance = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10; // affordable
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit =
      txs[0].tx.invocation.gas_limit * 2;        // > balance
  txs[2].tx.uniquifier.block_expiry_height = 12; // ordinary follow-up

  MempoolTestInfra server;
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();

  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/balance);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  // txs[0] is nominated; txs[1] is over budget, so [account] is excluded from
  // the block and dropped from [ready_accounts_] for the rest of the block.
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 0UL);

  // An ordinary follow-up submission lands on the excluded account's runqueue,
  // but [add_txs] refuses to re-insert the account into [ready_accounts_].
  scheduler->add_txs({txs[2]}, hash_txs({txs[2]}))();
  EXPECT_EQ(scheduler->size(), 1UL);
  ASSERT_NE(scheduler->find_tx(sha256(txs[2].tx)), nullptr);

  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled.empty()); // still excluded

  // Commit a block that touches the excluded account's runqueue tx.
  // In [remove_from_scheduler()], [account_is_ready()] is false and
  // [ready_accounts_.erase(...)] returns 0. Before the fix,
  // [ready_accounts_.erase(...)] would trigger a CHECK.
  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[2].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();

  // We only reach this point if the fix is working.
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[2].tx)), nullptr);
}

// A validated-block commit empties an excluded account's runqueue while the
// nomination is pending: remove_from_scheduler(), then abandon.
TEST(Scheduler, ValidatedCommitForPendingExcludedAccountLeavesAccountReady)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 4);
  const auto balance = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10; // affordable
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit =
      txs[0].tx.invocation.gas_limit * 2;        // > balance
  txs[2].tx.uniquifier.block_expiry_height = 12; // affordable
  txs[3].tx.uniquifier.block_expiry_height = 13; // submitted after commit

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/balance);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1], txs[2]},
                     hash_txs({txs[0], txs[1], txs[2]}))();
  EXPECT_EQ(scheduler->size(), 3UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 1UL);
  ASSERT_NE(scheduler->find_tx(sha256(txs[2].tx)), nullptr);

  x23::Vec<TransactionHash> committed;
  committed.push_back(sha256(txs[0].tx));
  committed.push_back(sha256(txs[2].tx));
  scheduler->remove_from_scheduler(
      x23::span<const TransactionHash>(committed))();
  scheduler->abandon_pending_nomination(/*block_id=*/1)();

  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[0].tx)), nullptr);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[2].tx)), nullptr);

  scheduler->add_txs({txs[3]}, hash_txs({txs[3]}))();
  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[3]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

// Same commit sequence as above, but the unaffordable tx is the account's
// first: nomination schedules nothing for the account, excludes it, and leaves
// the later affordable tx on the runqueue for a competing block to commit.
TEST(Scheduler, ValidatedCommitAfterUnaffordableHeadTxLeavesAccountReady)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto balance = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10;
  txs[0].tx.invocation.gas_limit =
      txs[1].tx.invocation.gas_limit * 2;        // > balance
  txs[1].tx.uniquifier.block_expiry_height = 11; // affordable
  txs[2].tx.uniquifier.block_expiry_height = 12; // submitted after commit

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/balance);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 1UL);
  ASSERT_NE(scheduler->find_tx(sha256(txs[1].tx)), nullptr);

  x23::Vec<TransactionHash> committed;
  committed.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(
      x23::span<const TransactionHash>(committed))();
  scheduler->abandon_pending_nomination(/*block_id=*/1)();

  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[1].tx)), nullptr);

  scheduler->add_txs({txs[2]}, hash_txs({txs[2]}))();
  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[2]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, ProcessTransfersUpdatesReceivedGroup)
{
  constexpr BlockHeight kBlock = 5;
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  const BlockTransfers transfer = one_chunk_transfer(
      kBlock,
      {Delta{.address = account,
             .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000))}});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(5'000'000'000));
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], kBlock);
}

TEST(Scheduler, ProcessTransfersCreatesHogAgeForFirstTimeRecipient)
{
  constexpr BlockHeight kBlock = 5;
  const ContractAddress account = make_account_address(99);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  const BlockTransfers transfer = one_chunk_transfer(
      kBlock,
      {Delta{.address = account,
             .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000))}});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(age->address, account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(5'000'000'000));
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], kBlock);
}

TEST(Scheduler, ApplyCommittedBlockHogAgesUpdatesLiveState)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/1,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->apply_committed_block_hog_ages({txs[0]}, /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), gas_limit);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 1);
}

TEST(Scheduler, ApplyCommittedBlockHogAgesSkipsAccountWhenGasExceedsLocalHogs)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  scheduler->apply_committed_block_hog_ages({txs[0]}, /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, ApplyCommittedBlockHogAgesSkipsOnlyInvalidAccounts)
{
  const ContractAddress &valid_account = MempoolTestInfra::kAccountAddresses[0];
  const ContractAddress &invalid_account =
      MempoolTestInfra::kAccountAddresses[1];
  fib_client_t valid_client(valid_account);
  fib_client_t invalid_client(invalid_account);
  const SignedTransactionBatch valid_txs =
      generate_unsigned_txs(valid_client, 1);
  const SignedTransactionBatch invalid_txs =
      generate_unsigned_txs(invalid_client, 1);
  const auto gas_limit =
      static_cast<uint128_t>(valid_txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(valid_account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(valid_account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(valid_account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({valid_txs[0], invalid_txs[0]},
                     hash_txs({valid_txs[0], invalid_txs[0]}))();
  scheduler->apply_committed_block_hog_ages({valid_txs[0], invalid_txs[0]},
                                            /*block_height=*/1)();

  {
    const auto [age, lock] = scheduler->get_cached_hog_age(valid_account);
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), gas_limit);
  }
  {
    const auto [age, lock] = scheduler->get_cached_hog_age(invalid_account);
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
              static_cast<uint128_t>(0));
  }
}

TEST(Scheduler, ApplyCommittedBlockHogAgesRollsBackAccountWhenLaterTxFails)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  scheduler->apply_committed_block_hog_ages({txs[0], txs[1]},
                                            /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), gas_limit);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, ProcessTransfersHandlesWithdrawalsAcrossGroups)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  constexpr auto kGas = static_cast<uint128_t>(5'000'000'000);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, kGas * 4);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0, kGas * 3);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, kGas * 2);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  const auto signed_gas = static_cast<int128_t>(kGas);
  const int128_t delta_value = -8 * signed_gas;
  const BlockTransfers transfer =
      one_chunk_transfer(10, {Delta{.address = account,
                                    .hog_delta = to_xdr(delta_value)}});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), kGas);
}

TEST(Scheduler, CycleAccountWhenGasInsufficient)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/1,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();

  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front().tx.invocation.invoked_address, account);

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 2);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(txs[0].tx.invocation.gas_limit));
}

TEST(Scheduler, FindTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]}, hash_txs({txs[0]}))();
  EXPECT_EQ(scheduler->size(), 1UL);

  TransactionHash hash = sha256(txs[0].tx);
  std::shared_ptr<const SignedTransaction> tx = scheduler->find_tx(hash);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(sha256(tx->tx), hash);
  EXPECT_EQ(tx->tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);
}

TEST(Scheduler, RemoveFromRunqueueSingleTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  // Validate a block.
  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  // Nominate a block.
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(sha256(scheduled.front().tx), sha256(txs[1].tx));
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  hashes = {sha256(txs[1].tx)};
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxsNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  EXPECT_EQ(scheduled_hashes.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, ReenqueueTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]}, hash_txs({txs[0], txs[1]}))();
  EXPECT_EQ(scheduler->size(), 2UL);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);

  scheduler->reenqueue_txs(scheduled_hashes.begin(), scheduled_hashes.end());
  EXPECT_EQ(scheduler->size(), 2UL);

  for (const TransactionHash &hash : scheduled_hashes) {
    std::shared_ptr<const SignedTransaction> reenqueued_tx =
        scheduler->find_tx(hash);
    ASSERT_NE(reenqueued_tx, nullptr);
  }
}

TEST(Scheduler, Fairness)
{
  constexpr size_t kNumRounds = 100;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(
        generate_unsigned_txs(fib_clients.back(), kBlockSize * kNumRounds));
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 3 and 5 billion hogs.
  // Account 1: Age 2 and 10 billion hogs.
  // Account 2: Age 1 and 15 billion hogs.
  // Account 3: Age 0 and 20 billion hogs.
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestInfra::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/MempoolTestInfra::kAccountAddresses.size() - i - 1,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const SignedTransactionBatch &tx : txs) {
    scheduler->add_txs(tx, hash_txs(tx))();
  }

  std::unordered_map<ContractAddress, size_t, SipHash24Hasher> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }

    // We simulate committing the block by increment the current block number in
    // the scheduler.
    scheduler->increment_current_block()();
  }

  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestInfra::kAccountAddresses[i];
    // The number of txs scheduled should be the proportion of total hogs that
    // this account has multiplied by [kBlockSize]. For example, account 1 has
    // 10 billion hogs, so its proportion of total hogs is 10 billion / (5
    // billion
    // + 10 billion + 15 billion + 20 billion) = 10 billion / 50 billion = 0.2.
    // Thus, the number of txs scheduled for account 1 should be 0.2 *
    // [kBlockSize] * [kNumRounds].
    //
    // We divide by 10 at the end rather than do (i + 1) / 10 * [kBlockSize] *
    // [kNumRounds] to avoid truncation due to integer division.
    const size_t target = (i + 1) * kBlockSize * kNumRounds / 10;
    EXPECT_LE(tx_counts[address],
              target + MempoolTestInfra::kAccountAddresses.size());
    EXPECT_GE(tx_counts[address],
              target - MempoolTestInfra::kAccountAddresses.size());
  }
}

TEST(Scheduler, FairnessAcrossBlocks)
{
  constexpr size_t kNumRounds = 100;
  static_assert(kNumRounds % 10 == 0);

  std::vector<fib_client_t> fib_clients;
  std::unordered_map<ContractAddress, SignedTransactionBatch, SipHash24Hasher>
      txs;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs[address] =
        generate_unsigned_txs(fib_clients.back(), kBlockSize * kNumRounds);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and
  // hogs:
  //
  // Account 0: Age 0 and 5 billion hogs.  (Will get 10% of the throughput.)
  // Account 1: Age 0 and 10 billion hogs. (Will get 20% of the throughput.)
  // Account 2: Age 0 and 15 billion hogs. (Will get 30% of the throughput.)
  // Account 3: Age 0 and 20 billion hogs. (Will get 40% of the throughput.)
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestInfra::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const auto &[_, tx] : txs) {
    scheduler->add_txs(tx, hash_txs(tx))();
  }

  std::unordered_map<ContractAddress, size_t, SipHash24Hasher> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
    EXPECT_EQ(scheduled.size(), 1UL);
    tx_counts[scheduled.front().tx.invocation.invoked_address]++;

    // We simulate committing the block by increment the current block number in
    // the scheduler.
    scheduler->increment_current_block()();
  }

  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestInfra::kAccountAddresses[i];
    const size_t target = (i + 1) * (kNumRounds / 10);
    EXPECT_LE(tx_counts[address],
              target + MempoolTestInfra::kAccountAddresses.size());
    EXPECT_GE(tx_counts[address],
              target - MempoolTestInfra::kAccountAddresses.size());
  }
}

TEST(Scheduler, UnschedulableHeadTxCrashesScheduler)
{
  const ContractAddress &account_a = MempoolTestInfra::kAccountAddresses[0];
  const ContractAddress &account_b = MempoolTestInfra::kAccountAddresses[1];

  fib_client_t fib_client_a(account_a);
  fib_client_t fib_client_b(account_b);

  MempoolTestInfra server;
  server.set_hogs(account_a, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/100);
  server.set_hogs(account_a, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account_a, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  constexpr char8_t kProgramName[] = u8"slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransaction tx1{
      .tx = Transaction{
          .invocation = fib_client_a.make_invocation<kProgramName>(
              /*gas_amount=*/200, kFibonacciN),
          .uniquifier = {.block_expiry_height = 1000, .nonce = 0},
          .contracts_to_deploy = {},
      }};

  SignedTransaction tx2{
      .tx = Transaction{
          .invocation = fib_client_a.make_invocation<kProgramName>(
              /*gas_amount=*/50, kFibonacciN),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {},
      }};

  SignedTransaction tx_b{
      .tx = Transaction{
          .invocation = fib_client_b.make_invocation<kProgramName>(
              /*gas_amount=*/5'000'000'000, kFibonacciN),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 0},
          .contracts_to_deploy = {},
      }};

  scheduler->add_txs({tx1, tx2, tx_b}, hash_txs({tx1, tx2, tx_b}))();

  {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    // Ensure that this does not trigger a CHECK in the scheduler (see issue
    // #1474).
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  }
}

} // namespace hogd
