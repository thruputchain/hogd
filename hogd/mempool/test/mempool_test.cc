#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "block.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_gateway.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "nomination.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 100;

void
set_cached_hog_age_to_zero(MempoolTestInfra &server,
                           const ContractAddress &account)
{
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  auto scheduler = server.mempool_coord_rpc().scheduler();
  auto [hog_age, lock] = scheduler->get_cached_hog_age(account);
  hog_age->num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(0));
  hog_age->average_ages[kHogAgeGroupToSchedule] = 0;
  hog_age->num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(static_cast<uint128_t>(0));
  hog_age->average_ages[kHogAgeGroupAlreadyScheduled] = 0;
  hog_age->num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(0));
  hog_age->average_ages[kHogAgeGroupReceived] = 0;
}

SchedulerValidationResult
validate_nominated_block_after_zeroing_local_scheduler_hogs(
    MempoolTestInfra &server, const SignedTransaction &tx)
{
  const ContractAddress &account = tx.tx.invocation.invoked_address;
  ValidatorID validator_id{};

  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{tx})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  CHECK_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  CHECK_EQ(res_nominate.outcome(),
           SchedulerNominationOutcome::SchedulerNominationOK);

  Nomination nomination = res_nominate.nominated_block().nomination;
  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  CHECK_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  server.mempool_coord_rpc().scheduler()->shut_down()();

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  SignedTransactionHashBatch tx_hashes;
  CHECK(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  CHECK_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  CHECK_EQ(db_tip.height, 1);

  nomination.block_hash =
      sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                         .block_height = nomination.block_height,
                         .txset_hash =
                             MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                         .db_hash = db_tip.db_hash,
                         .events_hash = db_tip.events_hash,
                         .prev_block_hash = nomination.prev_block_hash});

  set_cached_hog_age_to_zero(server, account);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{tx})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  CHECK_EQ(res_validate.outcome(),
           SchedulerValidationResult::SchedulerValidationOK);
  return res_validate;
}

} // namespace

// TODO(jhumphri): Update this test once we implement authentication and
// vetting.
TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(auth_data)}};
  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;
  txs[0].signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0));

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

TEST(Mempool, SubmitUnvettedMempoolMaxCapacityLimit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  size_t mempool_tx_size_limit = 1'000;
  SignedTransactionBatch txs =
      generate_unsigned_txs(fib_client, mempool_tx_size_limit + 1);

  MempoolTestInfra server(/*skip_genesis_block=*/false, mempool_tx_size_limit);

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(auth_data)}};
  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;

  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  const std::vector<ChunkRange> chunks =
      make_chunks(txs.size(), available_parallelism());
  std::vector<std::future<void>> futures;
  futures.reserve(chunks.size());

  for (const ChunkRange &chunk : chunks) {
    futures.push_back(worker_pool.submit([chunk, &txs, &sk] {
      for (size_t i = chunk.start; i < chunk.end; i++) {
        SignedTransaction &tx = txs[i];
        tx.tx.auth_claim = make_sign_claim(0);
        tx.signatures.push_back(make_signature(sk, tx.tx, /*public_key_id=*/0));
      }
    }));
  }
  for (std::future<void> &future : futures) {
    future.get();
  }

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs})();
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(std::move(batch))();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(),
            mempool_tx_size_limit);
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

TEST(Mempool, SubmitUnvettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;
  txs[0].signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0));

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, SubmitVettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, Nominate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.nominated_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.nominated_block().nomination.transactions_hash,
                            tx_hashes)());
    EXPECT_EQ(tx_hashes.size(), 1UL);
    EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));
  }
}

TEST(Mempool, NominateBatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.nominated_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), txs.size());
    std::set<SignedTransaction> parsed_txs_set(parsed_txs.begin(),
                                               parsed_txs.end());
    std::set<SignedTransaction> txs_set(txs.begin(), txs.end());
    EXPECT_EQ(parsed_txs_set, txs_set);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.nominated_block().nomination.transactions_hash,
                            tx_hashes)());
    EXPECT_EQ(tx_hashes.size(), txs.size());
    std::set<SignedTransactionHash> parsed_tx_hashes_set(tx_hashes.begin(),
                                                         tx_hashes.end());
    std::set<SignedTransactionHash> tx_hashes_set;
    for (const SignedTransaction &tx : txs) {
      EXPECT_TRUE(tx_hashes_set.insert(sha256(tx.tx)).second);
    }
    EXPECT_EQ(parsed_tx_hashes_set, tx_hashes_set);
  }
}

TEST(Mempool, NominateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.nominated_block().txs, parsed_txs)());
    EXPECT_TRUE(parsed_txs.empty());
  }
}

TEST(Mempool, BlockHeightsIncrement)
{
  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, i);

    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerNominationOutcome::SchedulerNominationOK);

    const Nomination &nomination = res_nominate.nominated_block().nomination;
    EXPECT_EQ(nomination.block_height, i + 1);

    auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
        res_nominate.nominated_block().block_id)();
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().height, kNumBlocks);
}

TEST(Mempool, NominateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(
        res_nominate.nominated_block().nomination.transactions_hash,
        tx_hashes)());
    EXPECT_EQ(tx_hashes.size(), 1UL);
    EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));
  }

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, CommitBlockNotFound)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      /*block_id=*/1)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitBlockNotFound);
}

TEST(Mempool, Validate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateMalformedNominationDecodeReturnsInvalid)
{
  MempoolTestInfra server;

  SignedTransactionHashBatch tx_hashes{SignedTransactionHash{}};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Nomination nomination{
      .block_height = db_tip.height + 1,
      .prev_block_hash = db_tip.block_hash,
      .transactions_hash = t_to_parallel_batch(tx_hashes)(),
      .transactions_nondeterministic_data =
          t_to_parallel_batch(tx_nondeterministic_data)(),
  };

  ASSERT_EQ(nomination.transactions_hash.segment_tokens.size(), 1UL);
  ASSERT_EQ(nomination.transactions_nondeterministic_data.segment_tokens.size(),
            1UL);
  nomination.transactions_hash.segment_tokens[0]++;
  nomination.transactions_nondeterministic_data.segment_tokens[0]++;

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

TEST(Mempool, ValidateMismatchedNominationCountsReturnsInvalid)
{
  MempoolTestInfra server;

  SignedTransactionHashBatch tx_hashes{SignedTransactionHash{},
                                       SignedTransactionHash{}};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Nomination nomination{
      .block_height = db_tip.height + 1,
      .prev_block_hash = db_tip.block_hash,
      .transactions_hash = t_to_parallel_batch(tx_hashes)(),
      .transactions_nondeterministic_data =
          t_to_parallel_batch(tx_nondeterministic_data)(),
  };

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

TEST(Mempool, ValidateBatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  DbTip db_tip{
      .height = 1, .block_hash = {}, .events_hash = {}, .db_hash = {}, .id = 0};
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, Nomination{.block_height = 1})();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateWrongBlockHeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    // Compute block_hash with a wrong block_height. This should cause
    // validation to fail because block_height is part of the block hash.
    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height + 1,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchRemoveTxFromNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});

    // Remove a transaction from the nomination.
    tx_hashes.pop_back();
    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nondeterministic_data)());
    tx_nondeterministic_data.pop_back();

    nomination.transactions_hash = t_to_parallel_batch(tx_hashes)();
    nomination.transactions_nondeterministic_data =
        t_to_parallel_batch(tx_nondeterministic_data)();
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchMissingTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, /*num_txs=*/5);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerNominationResult res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }

  // Remove a transaction.
  txs.pop_back();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMissingTxs);
}

TEST(Mempool, ValidateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, ValidateCommitUpdatesHogAgesWithoutNominatedBlock)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  ValidatorID validator_id{};

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  auto scheduler = server.mempool_coord_rpc().scheduler();
  auto [before_validation_commit, before_lock] =
      scheduler->get_cached_hog_age(account);
  const uint128_t to_schedule_before =
      from_xdr(before_validation_commit->num_hogs[kHogAgeGroupToSchedule]);
  const uint128_t already_scheduled_before = from_xdr(
      before_validation_commit->num_hogs[kHogAgeGroupAlreadyScheduled]);
  const BlockHeight already_scheduled_age_before =
      before_validation_commit->average_ages[kHogAgeGroupAlreadyScheduled];
  before_lock.unlock();

  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    ASSERT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  ASSERT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto [before_commit_again, before_commit_lock] =
      scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(before_commit_again->num_hogs[kHogAgeGroupToSchedule]),
            to_schedule_before);
  EXPECT_EQ(from_xdr(
                before_commit_again->num_hogs[kHogAgeGroupAlreadyScheduled]),
            already_scheduled_before);
  EXPECT_EQ(before_commit_again->average_ages[kHogAgeGroupAlreadyScheduled],
            already_scheduled_age_before);
  before_commit_lock.unlock();

  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  auto [after_validation_commit, after_lock] =
      scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(after_validation_commit->num_hogs[kHogAgeGroupToSchedule]),
            to_schedule_before);
  EXPECT_EQ(
      from_xdr(after_validation_commit->num_hogs[kHogAgeGroupAlreadyScheduled]),
      already_scheduled_before);
  EXPECT_EQ(after_validation_commit->average_ages[kHogAgeGroupAlreadyScheduled],
            db_tip.height + 1);
  after_lock.unlock();
}

TEST(Mempool, ValidateTwo)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // Now nominate another block.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.height, 2);

  res_nominate = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // Now validate another block.
  nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 3);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, GetTip)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.height, 0);

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.height, 1);

  res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip, res_validate.block().db_tip);
}

TEST(Mempool, CommitValidatedBlockSkipsInvalidHogAgeUpdate)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  MempoolTestInfra server;

  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  auto res_validate =
      validate_nominated_block_after_zeroing_local_scheduler_hogs(server,
                                                                  txs[0]);

  // Validation should still accept the block even though the local scheduler's
  // hog-age buckets cannot pay for the tx.
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  {
    auto [hog_age, lock] = scheduler->get_cached_hog_age(account);
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupAlreadyScheduled]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupReceived]),
              static_cast<uint128_t>(0));
  }

  const SchedulerCommitOutcome res_commit =
      server.mempool_coord_rpc().schedulercoord_commit_block(
          res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  {
    auto [hog_age, lock] = scheduler->get_cached_hog_age(account);
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupAlreadyScheduled]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(hog_age->num_hogs[kHogAgeGroupReceived]),
              static_cast<uint128_t>(0));
  }
}

TEST(Mempool, GenerateBlockId)
{
  uint64_t first = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(first, 0UL);

  uint64_t second = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(second, 0UL);
  EXPECT_NE(first, second);
}

TEST(Mempool, AuthChangedEvictsInvalidTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;

  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes_initial = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(auth_data)}};

  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;
  txs[0].signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0));

  // Submit the tx — should pass auth (weight 1 >= required 1)
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  // Now raise the required_weight to 10 — the tx (weight 1) no longer qualifies
  auth_data.required_weight = 10;
  auth_data.last_written_height = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes_update = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(auth_data)}};

  server.register_auth_data_updates(0, std::move(key_changes_initial))();
  server.register_auth_data_updates(1, std::move(key_changes_update))();

  // The auth_data_changed callback should re-verify and evict the tx
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, GenesisLoadGatewayGetTipDataRace)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());

  GenesisConfigurationBuilder builder;
  GenesisConfiguration genesis_configuration =
      builder.build(StorageCostModel::default_cost_model());

  constexpr size_t kWarmupReads = 100;
  std::atomic<bool> load_done = false;
  std::atomic<bool> failure = false;
  std::atomic<size_t> public_tip_reads = 0;
  std::latch start_latch(2);

  std::thread tip_reader([&]() {
    start_latch.arrive_and_wait();
    do {
      ExpectedDBTip tip = gateway.schedgateway_get_tip()();
      const RPCError error = tip.error();
      if (error != RPCError::kRPCNotFound && error != RPCError::kRPCNone) {
        failure.store(true, std::memory_order_release);
        break;
      }
      if (error == RPCError::kRPCNone && tip.db_tip().height != 0) {
        failure.store(true, std::memory_order_release);
        break;
      }
      public_tip_reads.fetch_add(1, std::memory_order_relaxed);
    } while (!load_done.load(std::memory_order_acquire));
  });

  std::thread genesis_loader([&]() {
    start_latch.arrive_and_wait();
    while (public_tip_reads.load(std::memory_order_acquire) < kWarmupReads &&
           !failure.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    DbSchedResult result =
        server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
            std::move(genesis_configuration))();
    if (result != DbSchedResult::DbSchedResultOK) {
      failure.store(true, std::memory_order_release);
    }
    load_done.store(true, std::memory_order_release);
  });

  genesis_loader.join();
  tip_reader.join();

  EXPECT_FALSE(failure.load(std::memory_order_acquire));
  EXPECT_GE(public_tip_reads.load(std::memory_order_relaxed), kWarmupReads);

  ExpectedDBTip final_tip = gateway.schedgateway_get_tip()();
  EXPECT_EQ(final_tip.error(), RPCError::kRPCNone);
  EXPECT_EQ(final_tip.db_tip().height, 0);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
