#include <gtest/gtest.h>

#include <memory>

#include "db_coord.hh"
#include "discovery.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "types.hh"

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
  const Ed25519SecretKey private_key = auth.get_tmp_key_pair().second;

  const Transaction &tx = txs[0];
  const SignedTransaction signed_tx = auth.sign(tx, private_key);
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
