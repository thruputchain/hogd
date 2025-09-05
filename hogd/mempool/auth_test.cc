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

TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);

  clients.interface->schedulercoord_submit_unvetted({txs[0]})();
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
