#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"

namespace hogd {

namespace {

int
block_explorer_init(const std::string &indexer_server_addr)
{
  // Register with the indexer server.
  indexer_block_explorer_client_t client =
      make_rpc_client<IndexerBlockExplorer1>(server_address_to_sockaddr(
                                                 indexer_server_addr),
                                             /*retry=*/true,
                                             LoadgenRpc::kRpcMaxMessageSize)();
  // client.event_indexer_intro(card)();

  return 0;
}

} // namespace

} // namespace hogd

// NOLINTBEGIN(bugprone-exception-escape)
int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  if (argc != 3) {
    std::println("usage: <indexer_server:port>]");
    return EXIT_FAILURE;
  }

  const std::string indexer_server_addr = argv[1];
  return hogd::block_explorer_init(indexer_server_addr);
};

// NOLINTEND(bugprone-exception-escape)
