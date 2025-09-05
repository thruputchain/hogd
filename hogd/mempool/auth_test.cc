#include <gtest/gtest.h>

#include <memory>

#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

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
  const Ed25519SecretKey private_key =
      auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                               /*idx=*/0);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, {{0, private_key}}, /*auxiliary_private_keys=*/{});
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
  const Ed25519SecretKey private_key = ed25519_keygen().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, {{0, private_key}}, /*auxiliary_private_keys=*/{});
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
  const Ed25519SecretKey private_key =
      auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                               /*idx=*/1);

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, {{0, private_key}}, /*auxiliary_private_keys=*/{});
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
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/1)},
      // We deliberately set [idx] to 3 below to test that everything works even
      // when public keys do not have contiguous indices.
      {3, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/3)}};

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, private_keys, /*auxiliary_private_keys=*/{});
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
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/1)},
      {3, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/3)}};
  private_keys[3] = ed25519_keygen().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, private_keys, /*auxiliary_private_keys=*/{});
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
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/1)},
      // We set the key to 4 but we pass 3 as the [idx] to
      // [create_tmp_key_pair()].
      {4, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/3)}};

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, private_keys, /*auxiliary_private_keys=*/{});
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
  std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/1)},
      {3, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/3)}};
  const Ed25519SecretKey &private_key_1 = private_keys[1];
  const Ed25519SecretKey &private_key_3 = private_keys[3];
  private_keys[1] = private_key_3;
  private_keys[3] = private_key_1;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, private_keys, /*auxiliary_private_keys=*/{});
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
  const std::unordered_map<uint32_t, Ed25519SecretKey> private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[0],
                                   /*idx=*/1)}};
  const std::unordered_map<uint32_t, Ed25519SecretKey> aux_1_private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[1],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[1],
                                   /*idx=*/1)}};
  const std::unordered_map<uint32_t, Ed25519SecretKey> aux_2_private_keys = {
      {0, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[2],
                                   /*idx=*/0)},
      {1, auth.create_tmp_key_pair(MempoolTestServer::kAccountAddresses[2],
                                   /*idx=*/1)}};

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx =
      auth.sign(tx, private_keys,
                /*auxiliary_private_keys=*/
                {{MempoolTestServer::kAccountAddresses[1], aux_1_private_keys},
                 {MempoolTestServer::kAccountAddresses[2],
                  aux_2_private_keys}});
  EXPECT_TRUE(auth.authenticate(signed_tx));
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
