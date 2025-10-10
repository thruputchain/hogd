#include <gtest/gtest.h>

#include <array>
#include <unordered_map>

#include "account.hh"
#include "auth_claim.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

auto make_self_sign_claim = [](uint32_t idx) {
  AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
  claim.invoked_address_signature_id() = idx;
  return claim;
};

auto make_other_sign_claim = [](uint32_t idx, ContractAddress address) {
  AuthClaim claim(AuthClaimType::kOtherAddressSignature);
  claim.other_address_signature_id().key_id = idx;
  claim.other_address_signature_id().address = address;
  return claim;
};

constexpr size_t kNumTxs = 100;

} // namespace

TEST(Authentication, SignAndVerify)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyNotEnoughNonceWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 10;
  set_nonce_key(auth_data, /*idx=*/0);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyNotEnoughGasWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/2);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyWrongNonceIndex)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/2);
  // adding the gas config; verification here should fail because of the missing
  // nonce, not the lack of gas
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyNoGasIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyNotEnoughGas)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/4'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyGasWrongIndex)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{1},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyNotEnoughGasWeightMultiConfig)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/2);
  add_gas_config(auth_data, /*gas=*/3'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyGasMultipleConfigs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  // claims not additive for single account
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/1'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  add_gas_config(auth_data, /*gas=*/2'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  add_gas_config(auth_data, /*gas=*/2'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyGasMultipleConfigsSomeInsufficient)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  // claims not additive for single account
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/7'000'000'000, /*keys=*/{0},
                 /*required_weight=*/2);
  add_gas_config(auth_data, /*gas=*/2'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{1},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyGasMultipleConfigsSomeInsufficient2)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  // claims not additive for single account
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/7'000'000'000, /*keys=*/{0},
                 /*required_weight=*/2);
  add_gas_config(auth_data, /*gas=*/2'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyGasZeroWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  // claims not additive for single account
  set_nonce_key(auth_data, /*idx=*/0);
  // can't accept key 0, but the claim is still valid because weight is 0
  add_gas_config(auth_data, /*gas=*/7'000'000'000, /*keys=*/{1},
                 /*required_weight=*/0);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMismatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               get_default_account());
  const Ed25519SecretKey private_key = ed25519_keygen().second;

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyWrongIndex)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/1, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSig)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1, 3},
                 /*required_weight=*/5);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigGasMultipleConfigs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  // this config doesn't get satisfied
  add_gas_config(auth_data, /*gas=*/1'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/4);
  // this config does get satisfied
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{3, 1},
                 /*required_weight=*/4);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigGasMultipleConfigsInsufficientWeights)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  // Exactly the same test as SignAndVerifyMultiSigGasMultipleConfigs,
  // but the amounts of allowed gas on the gas configs are swapped.
  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  // this config doesn't get satisfied
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/4);
  // this config does get satisfied
  add_gas_config(auth_data, /*gas=*/1'000'000'000, /*keys=*/{3, 1},
                 /*required_weight=*/4);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigGasMultipleConfigsWrongIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  add_gas_config(auth_data, /*gas=*/1'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{4, 1},
                 /*required_weight=*/5);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigNotEnoughGasWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 5;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/4);
  add_gas_config(auth_data, /*gas=*/4'000'000'000, /*keys=*/{3, 1},
                 /*required_weight=*/5);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigNotEnoughNonceWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 7;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  set_nonce_key(auth_data, /*idx=*/3);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigWrongNonceWeightIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 6;
  set_nonce_key(auth_data, /*idx=*/0);
  set_nonce_key(auth_data, /*idx=*/1);
  // We set [idx] to 2 rather than 3.
  set_nonce_key(auth_data, /*idx=*/2);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);

  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/2)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/3)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigMismatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  private_keys[3] = ed25519_keygen().second;

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigWrongIndex)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      // We set the key to 4 but we pass 3 as the [idx] to [create_key_pair()].
      {4, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(4));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigWrongIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                 /*required_weight=*/3);
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Ed25519SecretKey private_key_1 = private_keys[1];
  Ed25519SecretKey private_key_3 = private_keys[3];
  private_keys[1] = private_key_3;
  private_keys[3] = private_key_1;

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(make_self_sign_claim(3));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, private_keys,
                                        /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliary)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    if (i == 0) {
      auth_data.nonce_config.required_weight = 2;
      set_nonce_key(auth_data, /*idx=*/0);
      set_nonce_key(auth_data, /*idx=*/1);
    }
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/2);
    private_keys[i] = {
        {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/i + 1)},
        {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/i + 1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryNotEnoughNonceWeight)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    auth_data.nonce_config.required_weight = 3;
    set_nonce_key(auth_data, /*idx=*/0);
    set_nonce_key(auth_data, /*idx=*/1);
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/1);

    private_keys[i] = {
        {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/i + 1)},
        {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/i + 1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryWrongNonceWeightIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    auth_data.nonce_config.required_weight = 2;
    set_nonce_key(auth_data, /*idx=*/0);
    set_nonce_key(auth_data, /*idx=*/2);
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/1);

    private_keys[i] = {
        {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/i + 1)},
        {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/i + 1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryMismatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/1);
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }
  private_keys[2][0] = ed25519_keygen().second;

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryWrongIndex)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/1);
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }
  private_keys[1][2] = private_keys[1][1];
  CHECK_EQ(private_keys[1].erase(1), 1UL);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  // missing signature from this pk
  // tx.auth_claims.push_back(make_other_sign_claim(1,
  // MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryWrongIndices)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/1);
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }
  const Ed25519SecretKey private_key_0 = private_keys[1][0];
  const Ed25519SecretKey private_key_1 = private_keys[1][1];
  private_keys[1][0] = private_key_1;
  private_keys[1][1] = private_key_0;

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryNotEnoughGas)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    if (i == 0) {
      auth_data.nonce_config.required_weight = 2;
      set_nonce_key(auth_data, /*idx=*/0);
      set_nonce_key(auth_data, /*idx=*/1);
    }
    add_gas_config(auth_data, /*gas=*/4'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/2);
    private_keys[i] = {
        {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/i + 1)},
        {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/i + 1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }

  // This tx fails because the source account does not provide sufficient gas;
  // IF there were gas claims associated with the tx, then it could succeed
  // using gas sponsored by the other accounts.
  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx)());

  tx.gas_claims.push_back(
      GasClaim{.sponsored_contract = MempoolTestInfra::kAccountAddresses[1],
               .amount = 1'000'000'000});
  signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_TRUE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyAuthDataChanged)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  const SignedTransaction signed_tx =
      hogd::Authentication::sign_claims(tx, {{0, private_key}},
                                        /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx)());

  auth_data.nonce_config.required_weight = 2;
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  server.set_dbtip({.height = 1, .hash = {}, .id = 1});
  auth.update_block_id(/*block_id=*/1)();
  EXPECT_FALSE(auth.authenticate(signed_tx)());
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryAuthDataChanged)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs =
        generate_unsigned_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestInfra server;
  Authentication &auth = server.mempool_coord_rpc().authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  std::array<AccountAuthData, 3> auth_datas = {get_default_account(),
                                               get_default_account(),
                                               get_default_account()};
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData &auth_data = auth_datas[i];
    if (i == 0) {
      auth_data.nonce_config.required_weight = 2;
      set_nonce_key(auth_data, /*idx=*/0);
      set_nonce_key(auth_data, /*idx=*/1);
    }
    add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0, 1},
                   /*required_weight=*/2);
    private_keys[i] = {
        {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/i + 1)},
        {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/i + 1)}};
    server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[i],
                                 auth_data);
  }

  Transaction tx = txs[0];
  tx.auth_claims.push_back(make_self_sign_claim(0));
  tx.auth_claims.push_back(make_self_sign_claim(1));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(0, MempoolTestInfra::kAccountAddresses[2]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[1]));
  tx.auth_claims.push_back(
      make_other_sign_claim(1, MempoolTestInfra::kAccountAddresses[2]));
  const SignedTransaction signed_tx = hogd::Authentication::sign_claims(
      tx, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestInfra::kAccountAddresses[1], private_keys[1]},
       {MempoolTestInfra::kAccountAddresses[2], private_keys[2]}});
  EXPECT_TRUE(auth.authenticate(signed_tx)());

  auth_datas[1].public_keys[0].reset();
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[1],
                               auth_datas[1]);
  server.set_dbtip({.height = 1, .hash = {}, .id = 1});
  auth.update_block_id(/*block_id=*/1)();
  EXPECT_FALSE(auth.authenticate(signed_tx)());
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
