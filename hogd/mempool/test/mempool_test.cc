#include <gtest/gtest.h>

#include <memory>

#include "account.hh"
#include "auth_claim.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/mempool_test_server.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_account_auth_data(MempoolTestServer::kAccountAddresses[0],
                               auth_data);
  Transaction tx = txs[0].tx;
  auto make_sign_claim = [&](uint32_t idx) {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
    claim.invoked_address_signature_id() = idx;
    return claim;
  };
  tx.auth_claims.push_back(make_sign_claim(0));

  const SignedTransaction stx =
      hogd::Authentication::sign_claims(tx, 
                                 {{0, private_key}},
                                 /*auxiliary_private_keys=*/{});
  server.mempool_coord_rpc().schedulercoord_submit_unvetted({stx})();
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
}

TEST(Mempool, Nominate)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateBatch)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateEmpty)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);

  MempoolTestServer server;

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(),
            SchedulerNominationOutcome::SchedulerNominationError);
}

TEST(Mempool, Validate)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          {txs[0]})();
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateBatch)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateEmpty)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);

  MempoolTestServer server;

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                         Nomination{})();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchRemoveTxFromNomination)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  // Remove a transaction from the nomination.
  res_nominate.scheduler_nomination().nomination.transaction_hashes.pop_back();
  res_nominate.scheduler_nomination()
      .nomination.transaction_nominations.pop_back();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchMissingTx)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_txs(fib_client, /*num_txs=*/5);

  SchedulerNominationResult res_nominate;
  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  {
    MempoolTestServer server;

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    res_nominate = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerNominationOutcome::SchedulerNominationOK);
  }

  {
    // We reconstruct [server] because we need it to forget about the vetted txs
    // we gave it above. If we did not reconstruct [server], then removing a
    // transaction below from [txs] will have no effect because [server] will
    // still have the removed transaction as a vetted transaction.
    MempoolTestServer server;

    // Remove a transaction.
    txs.pop_back();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
        db_tip, res_nominate.scheduler_nomination().nomination)();
    EXPECT_EQ(res_validate.outcome(),
              SchedulerValidationResult::SchedulerValidationMissingTxs);
  }
}

TEST(Mempool, AuthChanged)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  // TODO(jhumphri): [generate_txs] should returned properly signed txs.
  // TODO(jhumphri): Move all of the configuration below to [MempoolTestServer].
  const SignedTransactionBatch txs =
      generate_txs(fib_client, MempoolCoordinatorRpc::kMaxBlockSize + 1);

  MempoolTestServer server;
  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
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
  server.set_account_auth_data(MempoolTestServer::kAccountAddresses[0],
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
    signed_txs.push_back(hogd::Authentication::sign_claims(tx,
                                                            {{0, private_key}},
                                                            /*auxiliary_private_keys=*/{}));
  }
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(signed_txs)();

  // Now change the [AccountAuthData].
  auth_data.nonce_config.required_weight = 2;
  server.set_account_auth_data(MempoolTestServer::kAccountAddresses[0],
                               auth_data);

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
  EXPECT_EQ(res.scheduler_nomination().nomination.transaction_hashes.size(),
            MempoolCoordinatorRpc::kMaxBlockSize);

  // Now check that the last tx is still nominated.
  db_tip = {.height = res.scheduler_nomination().nomination.block_height,
            .hash = res.scheduler_nomination().nomination.block_hash,
            .id = res.scheduler_nomination().block_id};
  res = server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
  EXPECT_EQ(res.scheduler_nomination().nomination.transaction_hashes.size(), 1);
}

TEST(Mempool, AuthInvalidated)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  // TODO(jhumphri): [generate_txs] should returned properly signed txs.
  // TODO(jhumphri): Move all of the configuration below to [MempoolTestServer].
  const SignedTransactionBatch txs =
      generate_txs(fib_client, MempoolCoordinatorRpc::kMaxBlockSize + 1);

  MempoolTestServer server;
  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
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
  server.set_account_auth_data(MempoolTestServer::kAccountAddresses[0],
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
    signed_txs.push_back(hogd::Authentication::sign_claims(tx,
                                                            {{0, private_key}},
                                                            /*auxiliary_private_keys=*/{}));
  }
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(signed_txs)();

  // Now change the [AccountAuthData].
  auth_data.nonce_config.required_weight = 10;
  server.set_account_auth_data(MempoolTestServer::kAccountAddresses[0],
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

TEST(Mempool, Stress)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
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