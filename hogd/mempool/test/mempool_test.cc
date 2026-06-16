#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "block.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/deploy_and_transfer.h"
#include "contract.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_gateway.h"
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

using deploy_and_transfer_client_t =
    ContractTraitClient<deploy_and_transfer::DeployAndTransfer>;

SHA256Hash
make_test_nonce(uint64_t value)
{
  return sha256(value);
}

std::tuple<uint64_t, uint64_t>
make_amount(uint128_t amount)
{
  return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
}

} // namespace

// TODO(jhumphri): Update this test once we implement authentication and
// vetting.
TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs at the same block height, which tests that the
// mempool scheduler can detect and discard duplicate txs. This does *not* test
// that hogdb detects duplicate uniquifiers.
TEST(Mempool, SubmitUnvettedDuplicateRejectedByMempool)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[0]})())();
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs each at a *different* block height, which tests
// that hogdb can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitUnvettedDuplicateRejectedByHogDB)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hogdb is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

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

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nondeterministic_data)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hogdb should reject
      // the tx's uniquifier as a duplicate, so hogexec should fail the tx with
      // error code [ExecResultTxNonDeterministicError]. The mempool should then
      // discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
        res_nominate.nominated_block().block_id)();
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().height, kNumBlocks);
}

TEST(Mempool, SubmitUnvettedMempoolMaxCapacityLimit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  size_t mempool_tx_size_limit = 1'000;
  SignedTransactionBatch txs =
      generate_unsigned_txs(fib_client, mempool_tx_size_limit + 1);

  MempoolTestInfra server(/*skip_genesis_block=*/false, mempool_tx_size_limit);

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

// This submits duplicate txs at the same block height, which tests that the
// mempool scheduler can detect and discard duplicate txs. This does *not* test
// that hogdb detects duplicate uniquifiers.
TEST(Mempool, SubmitVettedDuplicateRejectedByMempool)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id,
      t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[0]})())();
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs each at a *different* block height, which tests
// that hogdb can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitVettedDuplicateRejectedByHogDB)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  ValidatorID validator_id{};

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hogdb is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_vetted(
        validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

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

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nondeterministic_data)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hogdb should reject
      // the tx's uniquifier as a duplicate, so hogexec should fail the tx with
      // error code [ExecResultTxNonDeterministicError]. The mempool should then
      // discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
        res_nominate.nominated_block().block_id)();
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().height, kNumBlocks);
}

TEST(Mempool, SubmitUnvettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

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

TEST(Mempool, NominateTxWithError)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 2;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hogdb is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs. This is necessary to cause the tx to fail with
    // [ExecResultTxNonDeterministicError].
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

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

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nondeterministic_data)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hogdb should reject
      // the tx's uniquifier as a duplicate, so hogexec should fail the tx with
      // error code [ExecResultTxNonDeterministicError]. The mempool should then
      // discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
        res_nominate.nominated_block().block_id)();
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().height, kNumBlocks);
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

TEST(Mempool, NewlyDeployedContractHogCount)
{
  constexpr ContractAddress kDeployAndTransferAddress = make_account_address(7);
  constexpr uint128_t kTransferAmount = 100'000'000;
  const SHA256Hash deploy_nonce = make_test_nonce(1);
  std::shared_ptr<Contract> fibonacci_contract =
      load_contract("fibonacci.wasm");
  ASSERT_TRUE(fibonacci_contract);
  const SHA256Hash fibonacci_hash = sha256(*fibonacci_contract);
  const ContractAddress deployed_address =
      sha256(ContractDeploymentHashPreimage{.deployer_address =
                                                kDeployAndTransferAddress,
                                            .nonce = deploy_nonce,
                                            .contract_hash = fibonacci_hash});

  deploy_and_transfer_client_t deploy_and_transfer_client(
      kDeployAndTransferAddress);
  SignedTransaction tx{
      .tx = Transaction{
          .invocation =
              deploy_and_transfer_client.make_invocation<"deploy_and_transfer">(
                  /*gas_amount=*/5'000'000'000, deploy_nonce, fibonacci_hash,
                  make_amount(kTransferAmount)),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {*fibonacci_contract},
      }};

  MempoolTestInfra server;
  server.deploy_built_contract_to_address(kDeployAndTransferAddress,
                                          "deploy_and_transfer.wasm");

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{tx})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(get_tip.db_tip())();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(
      parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  EXPECT_EQ(parsed_txs[0], tx);

  SignedTransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.nominated_block().nomination.transactions_hash,
      tx_hashes)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  EXPECT_EQ(tx_hashes[0], sha256(tx.tx));

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  HogDBStatus<DeployedContractHash> deployed_metadata =
      server.read_address_metadata(deployed_address);
  ASSERT_TRUE(deployed_metadata.has_value());
  EXPECT_EQ(deployed_metadata->hash, fibonacci_hash);

  HogDBStatus<int128_t> deployed_balance =
      server.read_address_balance(deployed_address);
  ASSERT_TRUE(deployed_balance.has_value());
  EXPECT_GT(*deployed_balance, 0);

  server.mempool_coord_rpc().wait_for_transfer_data(
      res_nominate.nominated_block().nomination.block_height)();
  auto [deployed_hog_age, deployed_hog_age_lock] =
      server.mempool_coord_rpc().scheduler()->get_cached_hog_age(
          deployed_address);
  ASSERT_EQ(deployed_hog_age->address, deployed_address);
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  // The fibonacci contract needs to pay for storage, so the storage cost is
  // deducted from the live hogs used for scheduling. Thus, the number of hogs
  // in the [kHogAgeGroupReceived] scheduling group will be less than
  // [kTransferAmount].
  EXPECT_GT(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]), 0UL);
  EXPECT_LT(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]),
            kTransferAmount);
  HogDBStatus<int128_t> deployed_address_balance =
      server.read_address_balance(deployed_address);
  ASSERT_TRUE(deployed_address_balance.has_value());
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(*deployed_address_balance));
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

  // We reuse the [Nomination] struct.
  Nomination nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 0);
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

  // We reuse the [Nomination] struct.
  Nomination nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 0);
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateRejectsBlockWithFailedTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  SchedulerNominationResult res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  SchedulerCommitOutcome res_commit =
      server.mempool_coord_rpc().schedulercoord_commit_block(
          res_nominate.nominated_block().block_id)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

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

  // Submit the same tx again so that hogdb detects the duplicate tx and hogexec
  // fails the tx.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerValidationResult res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
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

  // We reuse the [Nomination] struct.
  Nomination nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 0);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
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

  // We reuse the [Nomination] struct.
  Nomination nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 0);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // Now nominate another block.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.height, 1);

  res_nominate = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // Now validate another block.
  nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);
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

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip, res_validate.block().db_tip);
  EXPECT_EQ(db_tip.height, 2);
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

  // We reuse the [Nomination] struct.
  Nomination nomination = res_nominate.nominated_block().nomination;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 0);
  }
  // The nominated block above also contains [txs[0]], we did not commit the
  // block, so [txs[0]] should not be detected as a duplicate.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.height, 0);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.block().db_tip.id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip, res_validate.block().db_tip);
  EXPECT_EQ(db_tip.height, 1);
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

TEST(Mempool, FailedAuthenticationTxExpiresAfterBlockDelta)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  txs[0].tx.auth_claim = make_sign_claim(0);
  const SignedTransactionHash tx_hash = sha256(txs[0].tx);

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);

  auto expect_validation_outcome = [&](auto expected_outcome) {
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

    SignedTransactionHashBatch tx_hashes{tx_hash};
    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
        NondeterministicProposalData{}};
    Nomination nomination{
        .block_height = get_tip.db_tip().height + 1,
        .prev_block_hash = get_tip.db_tip().block_hash,
        .transactions_hash = t_to_parallel_batch(tx_hashes)(),
        .transactions_nondeterministic_data =
            t_to_parallel_batch(tx_nondeterministic_data)(),
    };

    SchedulerValidationResult res_validate =
        server.mempool_coord_rpc().schedulercoord_validate(get_tip.db_tip(),
                                                           nomination)();
    EXPECT_EQ(res_validate.outcome(), expected_outcome);
    return res_validate;
  };

  for (size_t blocks_elapsed = 0;
       blocks_elapsed <
       MempoolCoordinatorRpc::kFailedAuthenticationTxsBlockExpiryDelta;
       blocks_elapsed++) {
    expect_validation_outcome(
        SchedulerValidationResult::SchedulerValidationInvalid);

    // Now commit an empty block.
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

    SchedulerNominationResult res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(get_tip.db_tip())();
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerNominationOutcome::SchedulerNominationOK);

    SchedulerCommitOutcome res_commit =
        server.mempool_coord_rpc().schedulercoord_commit_block(
            res_nominate.nominated_block().block_id)();
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerValidationResult res_validate = expect_validation_outcome(
      SchedulerValidationResult::SchedulerValidationMissingTxs);
  ASSERT_EQ(res_validate.missing_tx_hashes().size(), 1UL);
  EXPECT_EQ(res_validate.missing_tx_hashes()[0], tx_hash);
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

TEST(Mempool, SubmitUnvettedDuplicateTxDropped)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[1].tx.invocation = fib_client.make_invocation<"slow_fibonacci">(
      /*gas_amount=*/5'000'000'000, uint32_t{11});
  txs[1].tx.uniquifier = txs[0].tx.uniquifier;

  ASSERT_EQ(txs[0].tx.invocation.invoked_address,
            txs[1].tx.invocation.invoked_address);
  ASSERT_EQ(txs[0].tx.uniquifier.block_expiry_height,
            txs[1].tx.uniquifier.block_expiry_height);
  ASSERT_EQ(txs[0].tx.uniquifier.nonce, txs[1].tx.uniquifier.nonce);
  ASSERT_NE(sha256(txs[0].tx), sha256(txs[1].tx));

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(txs)())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 2UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 2UL);

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  SchedulerNominationResult res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(get_tip.db_tip())();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(
      parallel_batch_to_t(res_nominate.nominated_block().txs, parsed_txs)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  ASSERT_TRUE(parsed_txs[0] == txs[0] || parsed_txs[0] == txs[1]);

  const SignedTransactionHash tx0_hash = sha256(txs[0].tx);
  const SignedTransactionHash tx1_hash = sha256(txs[1].tx);

  SignedTransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.nominated_block().nomination.transactions_hash,
      tx_hashes)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  ASSERT_TRUE(tx_hashes[0] == tx0_hash || tx_hashes[0] == tx1_hash);

  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
  ASSERT_TRUE(
      parallel_batch_to_t(res_nominate.nominated_block()
                              .nomination.transactions_nondeterministic_data,
                          tx_nondeterministic_data)());
  EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);

  const SignedTransactionHash included_hash = tx_hashes[0];
  const SignedTransactionHash sibling_hash =
      included_hash == tx0_hash ? tx1_hash : tx0_hash;

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 2UL);
  EXPECT_TRUE(server.mempool_coord_rpc().scheduler()->find_tx(included_hash));
  EXPECT_TRUE(server.mempool_coord_rpc().scheduler()->find_tx(sibling_hash));

  SchedulerCommitOutcome res_commit =
      server.mempool_coord_rpc().schedulercoord_commit_block(
          res_nominate.nominated_block().block_id)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 0UL);
  EXPECT_FALSE(server.mempool_coord_rpc().scheduler()->find_tx(included_hash));
  EXPECT_FALSE(server.mempool_coord_rpc().scheduler()->find_tx(sibling_hash));
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
