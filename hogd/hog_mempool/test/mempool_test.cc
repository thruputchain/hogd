#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/deploy_and_transfer.h"
#include "built_contracts/fibonacci.h"
#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "header.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_db/status.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/rpc_from_gateway.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "parallel.hh"
#include "proposal.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "validator.hh"
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

ValidatorID
make_validator_id(uint8_t value)
{
  ValidatorID validator_id{};
  validator_id.fill(value);
  return validator_id;
}

SchedulerProposalResult
propose_block(MempoolTestInfra &server, const DbTip &parent,
              ProposalContext context = {})
{
  context.protocol = 0;
  return server.mempool_coord_rpc().schedulercoord_propose_block(
      parent.id, std::move(context))();
}

ConsensusValue
consensus_value(const DbTip &parent, const Proposal &proposal)
{
  return ConsensusValue{.protocol = 0,
                        .ext = {},
                        .prev_hash = sha256(parent.value),
                        .proposal_header = proposal.header,
                        .proposal_txs_hash = proposal_txs_compatibility_hash(
                            proposal.transactions),
                        .upgrades = {}};
}

SchedulerCommitOutcome
commit_block(MempoolTestInfra &server, const DbTip &parent,
             const SchedulerProposedBlock &proposed)
{
  return server.mempool_coord_rpc().schedulercoord_commit_block(
      proposed.block_id, consensus_value(parent, proposed.proposal))();
}

SchedulerCommitOutcome
commit_block(MempoolTestInfra &server, BlockID block_id, const DbTip &parent,
             const Proposal &proposal)
{
  return server.mempool_coord_rpc().schedulercoord_commit_block(
      block_id, consensus_value(parent, proposal))();
}

} // namespace

TEST(Mempool, ProposalTxsCompatibilityHashGoldenVector)
{
  ProposalTxs txs{};
  txs.hash.segment_tokens = {1, 2};
  txs.hash.segment_bytes = {4, 8};
  txs.hash.xdr = {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3};
  txs.nondeterministic_data.segment_tokens = {1};
  txs.nondeterministic_data.segment_bytes = {4};
  txs.nondeterministic_data.xdr = {0xde, 0xad, 0xbe, 0xef};

  const ProposalTxsHash expected = {
      0x3a, 0x3c, 0xe7, 0x82, 0x83, 0x94, 0x38, 0x26, 0xac, 0x43, 0x29,
      0x1f, 0xab, 0x79, 0xbd, 0x17, 0xc4, 0xad, 0x1d, 0x1e, 0xf9, 0xe2,
      0x2d, 0x8c, 0x72, 0x31, 0xa9, 0x7c, 0xb6, 0x68, 0xdc, 0xd9,
  };
  EXPECT_EQ(proposal_txs_compatibility_hash(txs), expected);
  EXPECT_NE(sha256(txs), expected);
}

TEST(Mempool, ShutdownBeforeReadyIsNoop)
{
  MempoolCoordinatorRpc coordinator(/*db_rpc=*/nullptr, /*auth_rpc=*/nullptr,
                                    NetworkHash{}, /*mempool_tx_size_limit=*/0);

  coordinator.schedulercoord_shutdown()();
}

TEST(Mempool, ExecutorCleanupFanoutDrainsBeforeRethrowingFirstException)
{
  MempoolTestInfra server;
  constexpr BlockID kBlock = std::numeric_limits<BlockID>::max() - 6;
  std::latch failure_started(1);
  std::latch blocked_started(1);
  std::latch allow_blocked_cleanup(1);
  std::atomic_size_t started{0};
  server.mempool_coord_rpc().set_finish_executor_started_test_hook(
      [&](BlockID block_id, size_t executor_idx) {
        EXPECT_EQ(block_id, kBlock);
        started.fetch_add(1, std::memory_order_relaxed);
        if (executor_idx == 0) {
          failure_started.count_down();
          throw std::runtime_error("first executor cleanup failure");
        }
        if (executor_idx == 1) {
          blocked_started.count_down();
          allow_blocked_cleanup.wait();
          throw std::runtime_error("blocked executor cleanup failure");
        }
        throw std::runtime_error("later executor cleanup failure");
      });

  // Run the cleanup on an explicit thread rather than std::async: libc++
  // detaches the std::async worker, so future::get() can return while the
  // worker is still tearing down coroutine frames that hold references to the
  // rethrown exception. That teardown frees the exception's what() buffer via
  // libc++abi's (TSAN-uninstrumented) refcounting, which TSAN reports as a
  // race against the EXPECT_STREQ below. Joining the thread before inspecting
  // the exception makes the ordering explicit.
  std::promise<void> cleanup_result;
  std::future<void> cleanup = cleanup_result.get_future();
  std::thread cleanup_thread([&] {
    try {
      server.mempool_coord_rpc().test_finish_all_executors(kBlock)();
      cleanup_result.set_value();
    } catch (...) {
      cleanup_result.set_exception(std::current_exception());
    }
  });
  failure_started.wait();
  blocked_started.wait();
  EXPECT_EQ(cleanup.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);

  allow_blocked_cleanup.count_down();
  cleanup_thread.join();
  bool caught_first_exception = false;
  try {
    cleanup.get();
  } catch (const std::runtime_error &e) {
    caught_first_exception = true;
    EXPECT_STREQ(e.what(), "first executor cleanup failure");
  }
  EXPECT_TRUE(caught_first_exception);
  EXPECT_EQ(started.load(std::memory_order_relaxed),
            MempoolCoordinatorRpc::kNumClientsPerExecutor);
  server.mempool_coord_rpc().set_finish_executor_started_test_hook({});
}

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
// that hog_db detects duplicate uniquifiers.
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
// that hog_db can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitUnvettedDuplicateRejectedByHogDb)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hog_db is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 1);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
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
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height, kNumBlocks);
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
// that hog_db detects duplicate uniquifiers.
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
// that hog_db can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitVettedDuplicateRejectedByHogDb)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  ValidatorID validator_id{};

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hog_db is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_vetted(
        validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 1);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
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
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height, kNumBlocks);
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

TEST(Mempool, Propose)
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
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.proposed_block().proposal.transactions.hash,
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
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), txs.size());
    std::set<SignedTransaction> parsed_txs_set(parsed_txs.begin(),
                                               parsed_txs.end());
    std::set<SignedTransaction> txs_set(txs.begin(), txs.end());
    EXPECT_EQ(parsed_txs_set, txs_set);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.proposed_block().proposal.transactions.hash,
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

TEST(Mempool, ProposalExceptionRollsBackPendingNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  const DbTip db_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()().db_tip();

  server.mempool_coord_rpc().set_propose_batch_result_test_hook(
      [](ExecProposeBatchResult &) {
        throw std::runtime_error("injected proposal failure");
      });
  EXPECT_THROW(propose_block(server, db_tip), std::runtime_error);
  server.mempool_coord_rpc().set_propose_batch_result_test_hook({});

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), txs.size());
  const SchedulerProposalResult retry = propose_block(server, db_tip);
  ASSERT_EQ(retry.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  SignedTransactionBatch proposed_txs;
  ASSERT_TRUE(parallel_batch_to_t(retry.proposed_block().txs, proposed_txs)());
  EXPECT_EQ(proposed_txs, txs);
}

TEST(Mempool, NominateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs)());
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
    // We submit each duplicate tx at a different block height so that hog_db is
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
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 1);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
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
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height, kNumBlocks);
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
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 1);

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height, kNumBlocks);
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
  auto res_nominate = propose_block(server, db_tip);
  EXPECT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(
        parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(
        res_nominate.proposed_block().proposal.transactions.hash, tx_hashes)());
    EXPECT_EQ(tx_hashes.size(), 1UL);
    EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));
  }

  auto res_commit =
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
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
          .invocation = deploy_and_transfer_client
                            .make_invocation<u8"deploy_and_transfer">(
                                /*gas_amount=*/5'000'000'000, deploy_nonce,
                                fibonacci_hash, make_amount(kTransferAmount)),
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
  auto res_nominate = propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(
      parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  EXPECT_EQ(parsed_txs[0], tx);

  SignedTransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.hash, tx_hashes)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  EXPECT_EQ(tx_hashes[0], sha256(tx.tx));

  auto res_commit =
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  HogDbStatus<DeployedContractHash> deployed_metadata =
      server.read_address_metadata(deployed_address);
  ASSERT_TRUE(deployed_metadata.has_value());
  EXPECT_EQ(deployed_metadata->hash, fibonacci_hash);

  HogDbStatus<int128_t> deployed_balance =
      server.read_address_balance(deployed_address);
  ASSERT_TRUE(deployed_balance.has_value());
  EXPECT_GT(*deployed_balance, 0);

  server.mempool_coord_rpc().wait_for_transfer_data(
      res_nominate.proposed_block().proposal.header.block_height)();
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
  HogDbStatus<int128_t> deployed_address_balance =
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
      /*block_id=*/1, ConsensusValue{})();
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
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateMalformedProposalDecodeReturnsInvalid)
{
  MempoolTestInfra server;

  SignedTransactionHashBatch tx_hashes{SignedTransactionHash{}};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height = db_tip.value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  ASSERT_EQ(proposal.transactions.hash.segment_tokens.size(), 1UL);
  ASSERT_EQ(proposal.transactions.nondeterministic_data.segment_tokens.size(),
            1UL);
  proposal.transactions.hash.segment_tokens[0]++;
  proposal.transactions.nondeterministic_data.segment_tokens[0]++;

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

TEST(Mempool, ValidateMismatchedProposalCountsReturnsInvalid)
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
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height = db_tip.value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
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
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
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
  SchedulerProposalResult res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SchedulerCommitOutcome res_commit =
      commit_block(server, db_tip, res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    ASSERT_EQ(db_tip.value.proposal_header.block_height, 1);
  }

  // Submit the same tx again so that hog_db detects the duplicate tx and
  // hog_exec fails the tx.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerValidationResult res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  const SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  Proposal proposal{};
  proposal.header.block_height =
      get_tip.db_tip().value.proposal_header.block_height + 1;
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate_block(
      get_tip.db_tip().id, std::move(proposal))();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateWrongTxsHash)
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
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);

    proposal.header.txs_hash[0] ^= 0xff;
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchRemoveTxFromProposal)
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
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Proposal] struct and make some adjustments for the
  // validation.
  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);

    // Remove a transaction from the proposal.
    tx_hashes.pop_back();
    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                    tx_nondeterministic_data)());
    tx_nondeterministic_data.pop_back();

    proposal.transactions.hash = t_to_parallel_batch(tx_hashes)();
    proposal.transactions.nondeterministic_data =
        t_to_parallel_batch(tx_nondeterministic_data)();
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
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

  SchedulerProposalResult res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }

  // Remove a transaction.
  txs.pop_back();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
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
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

// Regression test for a validation-gate leak: the
// [SchedulerValidationUnknownParentBlock] early return in
// [schedulercoord_validate_block()] used to skip
// [decrement_num_validations_in_progress()], leaking the gate increment.
// [close_validation_gate()] then waits forever for the count to reach zero,
// so every subsequent [schedulercoord_commit_block()] wedges. (The bench
// harness maps a timeout to success, so a wedged validator is silently
// masked in CI.) Without the fix this test HANGS at the commit below; with
// it, it completes in well under a second.
TEST(Mempool, UnknownParentBlockReleasesValidationGate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 32);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal proposal = res_nominate.proposed_block().proposal;

  // Validate against a parent id that is not the last committed block.
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id + 1,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationUnknownParentBlock);

  // The commit must not wedge on a leaked validation-gate increment.
  auto res_commit = commit_block(server, res_nominate.proposed_block().block_id,
                                 db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, ProposalUsesTransactionRoot)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  SchedulerProposalResult res_nominate = propose_block(server, parent_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal proposal = res_nominate.proposed_block().proposal;

  SchedulerCommitOutcome res_commit =
      commit_block(server, parent_tip, res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  SignedTransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes)());
  EXPECT_EQ(proposal.header.txs_hash,
            MempoolCoordinatorRpc::get_txset_hash(tx_hashes));
}

TEST(Mempool, ProposeBlockReturnsClosedRootsAndContext)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  ProposalContext unsupported_context{.protocol = 1};
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_propose_block(parent_tip.id,
                                              unsupported_context)()
                .outcome(),
            SchedulerProposalOutcome::SchedulerProposalError);

  ProposalContext context{.proposer = make_validator_id(0x42),
                          .block_timepoint = 123456789};
  SchedulerProposalResult res_propose =
      server.mempool_coord_rpc().schedulercoord_propose_block(parent_tip.id,
                                                              context)();
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const SchedulerProposedBlock &proposed = res_propose.proposed_block();

  EXPECT_EQ(proposed.proposal.header.proposer, context.proposer);
  EXPECT_EQ(proposed.proposal.header.block_timepoint, context.block_timepoint);
  EXPECT_NE(proposed.proposal.header.storage_hash, StorageShortHash{});

  ConsensusValue value = consensus_value(parent_tip, proposed.proposal);
  SchedulerCommitOutcome res_commit =
      server.mempool_coord_rpc().schedulercoord_commit_block(proposed.block_id,
                                                             value)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header, proposed.proposal.header);
}

TEST(Mempool, CommitBlockRejectsMismatchedConsensusValue)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  SchedulerProposalResult res_propose =
      server.mempool_coord_rpc().schedulercoord_propose_block(
          parent_tip.id, ProposalContext{})();
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const SchedulerProposedBlock &proposed = res_propose.proposed_block();

  const ConsensusValue expected =
      consensus_value(parent_tip, proposed.proposal);
  auto expect_rejected = [&](ConsensusValue value) {
    EXPECT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                  proposed.block_id, std::move(value))(),
              SchedulerCommitOutcome::SchedulerCommitInvalidConsensusValue);
  };

  ConsensusValue wrong_header = expected;
  wrong_header.proposal_header.events_hash[0] ^= 0xff;
  expect_rejected(std::move(wrong_header));

  ConsensusValue wrong_protocol = expected;
  wrong_protocol.protocol = 1;
  expect_rejected(std::move(wrong_protocol));

  ConsensusValue unexpected_upgrade = expected;
  unexpected_upgrade.upgrades.push_back(Upgrade{1});
  expect_rejected(std::move(unexpected_upgrade));

  EXPECT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                proposed.block_id, expected)(),
            SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().id, proposed.block_id);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            proposed.proposal.header.block_height);
}

TEST(Mempool, ValidateBlockRejectsTamperedHeaderRoots)
{
  auto expect_tampered_root_rejected = [](auto tamper) {
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    const SignedTransactionBatch txs =
        generate_unsigned_txs(fib_client, kNumTxs);

    MempoolTestInfra server;

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    const DbTip parent_tip = get_tip.db_tip();

    SchedulerProposalResult res_propose =
        server.mempool_coord_rpc().schedulercoord_propose_block(
            parent_tip.id, ProposalContext{})();
    ASSERT_EQ(res_propose.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);
    Proposal proposal = res_propose.proposed_block().proposal;
    tamper(proposal.header);

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

    SchedulerValidationResult res_validate =
        server.mempool_coord_rpc().schedulercoord_validate_block(
            parent_tip.id, std::move(proposal))();
    EXPECT_EQ(res_validate.outcome(),
              SchedulerValidationResult::SchedulerValidationInvalid);
  };

  expect_tampered_root_rejected(
      [](ProposalHeader &header) { header.storage_hash[0] ^= 0xff; });
  expect_tampered_root_rejected(
      [](ProposalHeader &header) { header.events_hash[0] ^= 0xff; });
}

TEST(Mempool, ValidateTwo)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // Now propose another block.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);

  res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // Now validate another block.
  proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
  res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.id, res_validate.block().block_id);
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);
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
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);
  }
  // The nominated block above also contains [txs[0]], we did not commit the
  // block, so [txs[0]] should not be detected as a duplicate.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 0);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.id, res_validate.block().block_id);
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
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
    Proposal proposal{
        .header = {.ext = {},
                   .network_hash = MempoolTestInfra::kNetworkHash,
                   .block_height =
                       get_tip.db_tip().value.proposal_header.block_height + 1},
        .transactions = {.ext = {},
                         .hash = t_to_parallel_batch(tx_hashes)(),
                         .nondeterministic_data =
                             t_to_parallel_batch(tx_nondeterministic_data)()}};

    SchedulerValidationResult res_validate =
        server.mempool_coord_rpc().schedulercoord_validate_block(
            get_tip.db_tip().id, proposal)();
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

    SchedulerProposalResult res_nominate =
        propose_block(server, get_tip.db_tip());
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    SchedulerCommitOutcome res_commit =
        commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
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
      builder.build_or_panic(StorageCostModel::default_cost_model());

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
      if (error == RPCError::kRPCNone &&
          tip.db_tip().value.proposal_header.block_height != 0) {
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
  EXPECT_EQ(final_tip.db_tip().value.proposal_header.block_height, 0);
}

TEST(Mempool, GenesisV2CeremonyMintsDeploysAndIsIdempotent)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  const StorageCostModel cost_model = StorageCostModel::default_cost_model();

  GenesisConfigurationBuilder builder;
  auto fibonacci_contract = load_contract("fibonacci.wasm");
  const SHA256Hash deployment_nonce = make_test_nonce(9001);
  const int128_t allocation = 1'000'000'000;
  using fibonacci_meta = ContractTraitMeta<fibonacci::Fibonacci>;
  builder.add_initial_config(fibonacci_contract, deployment_nonce,
                             fibonacci_meta::entrypoint<u8"fibonacci">(),
                             fibonacci_meta::make_calldata<u8"fibonacci">(
                                 uint32_t{0}),
                             allocation);

  auto root_contract = load_stock_genesis_root_contract();
  GenesisConfigurationV2 genesis =
      builder.build_v2(MempoolTestInfra::kNetworkHash, cost_model,
                       *root_contract);
  ASSERT_EQ(server.mempool_coord_rpc().schedcoord_load_genesis_configuration_v2(
                std::move(genesis))(),
            DbSchedResult::DbSchedResultOK);

  const SchedulerGetTipResult parent =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(parent.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  ASSERT_EQ(parent.db_tip().value.proposal_header.block_height, 0);

  SignedTransactionBatch ceremony_txs;
  ceremony_txs.append_range(
      builder.make_genesis_ceremony_transactions(cost_model));
  ParallelSignedTransactionBatch parallel = t_to_parallel_batch(ceremony_txs)();
  GenesisCeremonyRequest request{
      .expected_parent_height = 0,
      .expected_parent_value_hash = sha256(parent.db_tip().value),
      .expected_batch_hash = sha256(parallel),
      .context = ProposalContext{.protocol = 0},
      .transactions = parallel,
  };

  GenesisCeremonyResult result =
      server.mempool_coord_rpc().schedulercoord_commit_genesis_ceremony(
          request)();
  ASSERT_EQ(result.outcome(), GenesisCeremonyOutcome::GenesisCeremonyCommitted);
  EXPECT_EQ(result.commit().db_tip.value.proposal_header.block_height, 1);

  const ContractAddress deployed =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*fibonacci_contract), deployment_nonce);
  ASSERT_TRUE(server.read_address_metadata(deployed).has_value());
  ASSERT_TRUE(server.read_address_balance(deployed).has_value());
  EXPECT_GT(*server.read_address_balance(deployed), 0);
  ASSERT_TRUE(
      server.read_address_balance(kGenesisRootContractAddress).has_value());
  EXPECT_EQ(*server.read_address_balance(kGenesisRootContractAddress), 0);

  GenesisCeremonyResult retry =
      server.mempool_coord_rpc().schedulercoord_commit_genesis_ceremony(
          std::move(request))();
  EXPECT_EQ(retry.outcome(),
            GenesisCeremonyOutcome::GenesisCeremonyAlreadyCommitted);
  EXPECT_EQ(retry.commit().db_tip, result.commit().db_tip);
}

TEST(Mempool, SubmitUnvettedDuplicateTxDropped)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[1].tx.invocation = fib_client.make_invocation<u8"slow_fibonacci">(
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

  SchedulerProposalResult res_nominate =
      propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(
      parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  ASSERT_TRUE(parsed_txs[0] == txs[0] || parsed_txs[0] == txs[1]);

  const SignedTransactionHash tx0_hash = sha256(txs[0].tx);
  const SignedTransactionHash tx1_hash = sha256(txs[1].tx);

  SignedTransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.hash, tx_hashes)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  ASSERT_TRUE(tx_hashes[0] == tx0_hash || tx_hashes[0] == tx1_hash);

  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.nondeterministic_data,
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
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
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
