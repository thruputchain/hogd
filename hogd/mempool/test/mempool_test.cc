#include <gtest/gtest.h>
#include <set>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "block.hh"
#include "discovery.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23/x23.h"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 100;

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

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
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
