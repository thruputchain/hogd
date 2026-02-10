#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/init.h"
#include "port.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
auth_init(const std::string &mempool_address)
{
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }

  auto mempool_client =
      make_rpc_client<MempoolAuth1>(host_port, /*retry=*/true)();

  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);

  const std::string auth_db_address =
      std::format("{}:{}", hostname, kAuthDbServerPort);
  const std::string auth_mempool_address =
      std::format("{}:{}", hostname, kAuthMempoolServerPort);

  AuthCard auth_card{.protocol = version::kHogdProtocol(),
                     .upid = {},
                     .address_for_db = auth_db_address.c_str(),
                     .address_for_mempool = auth_mempool_address.c_str()};

  IntroAuthResult intro = mempool_client.mempoolauth_intro_auth(auth_card)();
  if (intro.shards().empty()) {
    throw std::runtime_error("No db shards registered with mempool");
  }

  const ShardCard &shard = intro.shards()[0];
  dbauth_client_t dbauth_client =
      make_dbauth_client(shard.address_for_auth.c_str())();

  AuthServer auth_server(std::move(dbauth_client));
  auth_server.set_mempool_client(std::move(mempool_client));
  AuthRpcServers rpc_servers(auth_server);
  CHECK_EQ(rpc_servers.start_servers(kAuthDbServerPort,
                                     kAuthMempoolServerPort),
           (std::pair<uint16_t, uint16_t>{kAuthDbServerPort,
                                          kAuthMempoolServerPort}));

  auth_server.subscribe_to_auth_data_changes(
      rpc_servers.make_auth_card(hostname))();

  info("auth server initialization complete.");
  std::promise<void>().get_future().wait();
  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

void
usage(char *argv[])
{
  std::println("usage: {} mempool_ip:port", argv[0]);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting auth server!");

  if (argc != 2) {
    usage(argv);
  }

  const std::string mempool_address = argv[1];
  return hogd::auth_init(mempool_address);
}
