#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/shared.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "indexer.hh"

namespace hogd {

namespace {

constexpr uint16_t kIndexerEventServerPort = 8060;

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
  // TODO(jhumphri): Initialize [RpcServer] for the indexer_event server.

  // Register with the event server.
  event_indexer_client_t client = create_client<EventIndexer1>(
      server_address_to_sockaddr(event_server_addr));
  const std::string indexer_server_addr =
      create_address(/*port=*/kIndexerEventServerPort);
  const IndexerCard card{.protocol = version::kHogdProtocol(),
                         .address_for_event = indexer_server_addr.c_str()};
  client.event_indexer_intro(card)();

  // TODO(jhumphri): Initialize [RpcServer] for indexer_block_explorer server.
  return 0;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  if (argc != 3) {
    std::println("usage: db_path event_server_ip:port]");
    return EXIT_FAILURE;
  }

  const std::string path = argv[1];
  const std::string event_server_addr = argv[2];
  return hogd::indexer_init(path, event_server_addr);
};
