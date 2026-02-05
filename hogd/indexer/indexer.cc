#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/shared.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"

namespace hogd {

namespace {

constexpr uint16_t kIndexerEventServerPort = 8060;
constexpr uint16_t kIndexerBlockExplorerServerPort = 8061;

// TODO(jhumphri): This is the same as in other subsystems.
std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

int
indexer_init(const std::string &path, const std::string &event_server_addr)
{
  IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(path);
  if (!db_status) {
    panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
  }
  IndexDB &db = db_status.value();

  // Create the RPC server that the event server talks to.
  RpcServer<IndexerEventRpc> event(db);
  event->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  CHECK_EQ(event.start_server(kIndexerEventServerPort),
           uint16_t{kIndexerEventServerPort});

  // Register with the event server.
  event_indexer_client_t client = make_rpc_client<EventIndexer1>(
      server_address_to_sockaddr(event_server_addr), /*retry=*/true)();
  const std::string indexer_server_addr =
      create_address(/*port=*/kIndexerEventServerPort);
  const IndexerCard card{.protocol = version::kHogdProtocol(),
                         .address_for_event = indexer_server_addr.c_str()};
  client.event_indexer_intro(card)();

  // Create the RPC server that the block explorers talk to.
  RpcServer<IndexerBlockExplorerRpc> block_explorer(db);
  block_explorer->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  CHECK_EQ(block_explorer.start_server(kIndexerBlockExplorerServerPort),
           uint16_t{kIndexerBlockExplorerServerPort});

  // The indexer is initialized. Now block forever.
  info("indexer initialization complete.");
  std::promise<void>().get_future().wait();

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
    std::println("usage: <event_server:port> <db_path>]");
    return EXIT_FAILURE;
  }

  const std::string event_server_addr = argv[1];
  const std::string path = argv[2];
  return hogd::indexer_init(path, event_server_addr);
};

// NOLINTEND(bugprone-exception-escape)
