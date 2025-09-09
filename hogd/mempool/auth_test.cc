#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>

#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

AccountAuthData
get_default_account()
{
  static std::array<PublicKeyConfig, MAX_SIGNERS> public_keys;
  if (public_keys.empty()) {
    // Initialize [public_keys].
    for (size_t i = 0; i < MAX_SIGNERS; i++) {
      public_keys[i].valid = false;
    }
  }
  CHECK_EQ(public_keys.size(), MAX_SIGNERS);
  constexpr NonceConfig nonce_config{.accepted_keys = 0, .required_weight = 0};

  return AccountAuthData{.public_keys = public_keys,
                         .gas_permissions = {},
                         .nonce_config = nonce_config};
}

Ed25519SecretKey
create_key_pair(AccountAuthData &auth_data, size_t idx, Weight weight)
{
  const auto &[public_key, private_key] = ed25519_keygen();
  PublicKeyConfig &pk_config = auth_data.public_keys[idx];
  pk_config.valid = true;
  pk_config.key = PublicKey(CryptographicScheme::Ed25519);
  pk_config.key.ed25519() = public_key;
  return private_key;
}

void
set_nonce_key(AccountAuthData &auth_data, size_t idx)
{
  CHECK_LT(idx, sizeof(auth_data.nonce_config.accepted_keys));
  auth_data.nonce_config.accepted_keys |= (1 << idx);
}

} // namespace

TEST(Authentication, SignAndVerify)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, {{0, private_key}},
                                 /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMismatch)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  auth.add_account(MempoolTestServer::kAccountAddresses[0],
                   get_default_account());
  const Ed25519SecretKey private_key = ed25519_keygen().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, {{0, private_key}},
                                 /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyWrongIndex)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/1, /*weight=*/1);
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, {{0, private_key}},
                                 /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSig)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, private_keys,
                                 /*auxiliary_private_keys=*/{});
  EXPECT_TRUE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigMismatch)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);
  private_keys[3] = ed25519_keygen().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, private_keys,
                                 /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigWrongIndex)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      // We set the key to 4 but we pass 3 as the [idx] to [create_key_pair()].
      {4, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, private_keys,
                                 /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigWrongIndices)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();
  AccountAuthData auth_data = get_default_account();
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
      {1, create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)},
      {3, create_key_pair(auth_data, /*idx=*/3, /*weight=*/1)}};
  auth.add_account(MempoolTestServer::kAccountAddresses[0], auth_data);

  const Ed25519SecretKey &private_key_1 = private_keys[1];
  const Ed25519SecretKey &private_key_3 = private_keys[3];
  private_keys[1] = private_key_3;
  private_keys[3] = private_key_1;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      hogd::Authentication::sign(tx, /*signature_weight=*/1, private_keys,
                                 /*auxiliary_private_keys=*/{});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliary)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    auth.add_account(MempoolTestServer::kAccountAddresses[i], auth_data);
  }

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx = hogd::Authentication::sign(
      tx, /*signature_weight=*/1, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestServer::kAccountAddresses[1], private_keys[1]},
       {MempoolTestServer::kAccountAddresses[2], private_keys[2]}});
  EXPECT_TRUE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryMismatch)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    auth.add_account(MempoolTestServer::kAccountAddresses[i], auth_data);
  }
  private_keys[2][0] = ed25519_keygen().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx = hogd::Authentication::sign(
      tx, /*signature_weight=*/1, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestServer::kAccountAddresses[1], private_keys[1]},
       {MempoolTestServer::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryWrongIndex)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    auth.add_account(MempoolTestServer::kAccountAddresses[i], auth_data);
  }
  private_keys[1][2] = private_keys[1][1];
  CHECK_EQ(private_keys[1].erase(1), 1UL);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx = hogd::Authentication::sign(
      tx, /*signature_weight=*/1, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestServer::kAccountAddresses[1], private_keys[1]},
       {MempoolTestServer::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

TEST(Authentication, SignAndVerifyMultiSigAuxiliaryWrongIndices)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  x23::Vec<Transaction> txs;
  txs.reserve(kNumTxs);
  {
    const SignedTransactionBatch signed_txs = generate_txs(fib_client, kNumTxs);
    for (const SignedTransaction &signed_tx : signed_txs) {
      txs.push_back(signed_tx.tx);
    }
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Authentication &auth = clients.interface->authentication();

  std::array<std::unordered_map<uint32_t, Ed25519SecretKey>, 3> private_keys;
  for (size_t i = 0; i < 3; i++) {
    AccountAuthData auth_data = get_default_account();
    private_keys[i] = {{0, create_key_pair(auth_data, /*idx=*/0, /*weight=*/1)},
                       {1,
                        create_key_pair(auth_data, /*idx=*/1, /*weight=*/1)}};
    auth.add_account(MempoolTestServer::kAccountAddresses[i], auth_data);
  }
  const Ed25519SecretKey private_key_0 = private_keys[1][0];
  const Ed25519SecretKey private_key_1 = private_keys[1][1];
  private_keys[1][0] = private_key_1;
  private_keys[1][1] = private_key_0;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx = hogd::Authentication::sign(
      tx, /*signature_weight=*/1, private_keys[0],
      /*auxiliary_private_keys=*/
      {{MempoolTestServer::kAccountAddresses[1], private_keys[1]},
       {MempoolTestServer::kAccountAddresses[2], private_keys[2]}});
  EXPECT_FALSE(auth.authenticate(signed_tx));
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
