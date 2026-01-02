#include <gtest/gtest.h>

#include "account.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "discovery.hh"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
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

  AccountAuthData auth_data = get_default_account();
  auth_data.last_written_height = 0;
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AccountAuthData>(auth_data)}};
  server.register_auth_data_updates(
      /*height=*/0, key_changes)();
  Transaction &tx = txs[0].tx;
  auto make_sign_claim = [&](uint32_t idx) {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
    claim.invoked_address_signature_id() = idx;
    return claim;
  };
  tx.auth_claims.push_back(make_sign_claim(0));

  server.mempool_coord_rpc().schedulercoord_submit_unvetted({txs[0]})();
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
}

TEST(Mempool, Nominate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateBatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.block_id())();
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
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  db_tip.height++;
  db_tip.hash = nomination.block_hash;
  db_tip.id++;

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
      nomination.transaction_hashes,
      /*prev_block_hash=*/nomination.prev_block_hash);
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
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
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  db_tip.height++;
  db_tip.hash = nomination.block_hash;
  db_tip.id++;

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
      nomination.transaction_hashes,
      /*prev_block_hash=*/nomination.prev_block_hash);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                         Nomination{})();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchRemoveTxFromNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  db_tip.height++;
  db_tip.hash = nomination.block_hash;
  db_tip.id++;

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
      nomination.transaction_hashes,
      /*prev_block_hash=*/nomination.prev_block_hash);

  // Remove a transaction from the nomination.
  nomination.transaction_hashes.pop_back();
  nomination.transaction_nominations.pop_back();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchMissingTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, /*num_txs=*/5);

  Nomination nomination;
  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  {
    MempoolTestInfra server;

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    SchedulerNominationResult res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerNominationOutcome::SchedulerNominationOK);
    nomination = server.coord_sched_rpc().get_nomination()().nomination;
  }

  {
    // We reconstruct [server] because we need it to forget about the vetted txs
    // we gave it above. If we did not reconstruct [server], then removing a
    // transaction below from [txs] will have no effect because [server] will
    // still have the removed transaction as a vetted transaction.
    MempoolTestInfra server;

    // Remove a transaction.
    txs.pop_back();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    auto res_validate =
        server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                           nomination)();
    EXPECT_EQ(res_validate.outcome(),
              SchedulerValidationResult::SchedulerValidationMissingTxs);
  }
}

TEST(Mempool, ValidateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  db_tip.height++;
  db_tip.hash = nomination.block_hash;
  db_tip.id++;

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
      nomination.transaction_hashes,
      /*prev_block_hash=*/nomination.prev_block_hash);
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.db_tip().id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, GetTip)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  DbTip db_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(db_tip.height, 1);

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  // We nominate a block first and then reuse the returned [Nomination] struct
  // for the validation below. The [Nomination] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // We reuse the [Nomination] struct and make some adjustments for the
  // validation.
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  db_tip.height++;
  db_tip.hash = nomination.block_hash;
  db_tip.id++;

  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
      nomination.transaction_hashes,
      /*prev_block_hash=*/nomination.prev_block_hash);
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  db_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(db_tip.height, 1);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_validate.db_tip().id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  db_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(db_tip, res_validate.db_tip());
}

TEST(Mempool, GenerateBlockId)
{
  uint64_t first = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(first, 0UL);

  uint64_t second = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(second, 0UL);
  EXPECT_NE(first, second);
}

// [kMaxBlockSize] is 10,000, which causes this test to time out. We should
// re-enable this test once we add performance improvements.
// NOLINTNEXTLINE
TEST(Mempool, AuthChanged)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  // TODO(jhumphri): [generate_unsigned_txs] should returned properly signed
  // txs.
  // TODO(jhumphri): Move all of the configuration below to [MempoolTestInfra].
  SignedTransactionBatch txs =
      generate_unsigned_txs(fib_client,
                            /*MempoolCoordinatorRpc::kMaxBlockSize +*/ 1);

  MempoolTestInfra server;
  server.set_hogs(MempoolTestInfra::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000 *
                      MempoolCoordinatorRpc::kMaxBlockSize * 2);

  AccountAuthData auth_data = get_default_account();
  auth_data.last_written_height = 0;
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AccountAuthData>(auth_data)}};
  server.register_auth_data_updates(
      /*height=*/0, key_changes)();
  Transaction tx = txs[0].tx;
  auto make_sign_claim = [&](uint32_t idx) {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
    claim.invoked_address_signature_id() = idx;
    return claim;
  };

  for (SignedTransaction &stx : txs) {
    stx.tx.auth_claims.push_back(make_sign_claim(0));
  }
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();

  // Now change the [AccountAuthData].
  auth_data.nonce_config.required_weight = 2;
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
  Nomination nomination =
      server.coord_sched_rpc().get_nomination()().nomination;
  EXPECT_EQ(nomination.transaction_hashes.size(),
            MempoolCoordinatorRpc::kMaxBlockSize);

  // Now check that the last tx is still nominated.
  db_tip = {.height = nomination.block_height,
            .hash = nomination.block_hash,
            .id = res.block_id()};
  res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
  nomination = server.coord_sched_rpc().get_nomination()().nomination;
  EXPECT_EQ(nomination.transaction_hashes.size(), 1);
}

// TODO(geoff): Pending executor auth data, and/or checking the updated auth
// data in the scheduler. NOLINTNEXTLINE
#if 0
TEST(Mempool, AuthInvalidated)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  // TODO(jhumphri): [generate_unsigned_txs] should returned properly signed
  // txs.
  // TODO(jhumphri): Move all of the configuration below to [MempoolTestInfra].
  const SignedTransactionBatch txs =
      generate_unsigned_txs(fib_client,
                            MempoolCoordinatorRpc::kMaxBlockSize + 1);

  MempoolTestInfra server;
  server.set_hogs(MempoolTestInfra::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000 *
                      MempoolCoordinatorRpc::kMaxBlockSize * 2);

  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/2);
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  SignedTransactionBatch signed_txs;
  for (const SignedTransaction &stx : txs) {
    auto make_sign_claim = [&](uint32_t idx) {
      AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
      claim.invoked_address_signature_id() = idx;
      return claim;
    };
    Transaction tx = stx.tx;
    tx.auth_claims.push_back(make_sign_claim(0));
    signed_txs.push_back(
        hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                          /*auxiliary_private_keys=*/{}));
  }
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(signed_txs)();

  // Now change the [AccountAuthData].
  auth_data.nonce_config.required_weight = 10;
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
  EXPECT_EQ(res.scheduler_nomination().nomination.transaction_hashes.size(),
            MempoolCoordinatorRpc::kMaxBlockSize);

  // Now check that the last tx is not nominated.
  db_tip = {.height = res.scheduler_nomination().nomination.block_height,
            .hash = res.scheduler_nomination().nomination.block_hash,
            .id = res.scheduler_nomination().block_id};
  res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  // [schedulercoord_nominate()] returns an error if there are no vetted txs to
  // nominate.
  EXPECT_EQ(res.outcome(),
            SchedulerNominationOutcome::SchedulerNominationError);
}
#endif

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
