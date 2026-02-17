#include <cstdlib>
#include <cstdint>
#include <format>
#include <future>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "sched_db.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
auth_init(const std::string &mempool_address, uint16_t db_port,
          uint16_t mempool_port)
{
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }

  sched_auth_client_t mempool_client =
      make_rpc_client<MempoolAuth1>(host_port, /*retry=*/true)();

  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);

  const std::string pre_intro_db_address = std::format("{}:{}", hostname, db_port);
  const std::string pre_intro_mempool_address =
      std::format("{}:{}", hostname, mempool_port);
  AuthCard pre_intro_card{.protocol = version::kHogdProtocol(),
                          .upid = {},
                          .address_for_db = pre_intro_db_address.c_str(),
                          .address_for_mempool =
                              pre_intro_mempool_address.c_str()};
  IntroAuthResult intro = mempool_client.mempoolauth_intro_auth(pre_intro_card)();
  if (intro.shards().empty()) {
    throw std::runtime_error("No db shards registered with mempool");
  }
  ShardCard &shard = intro.shards()[0];
  const std::string address_for_auth = shard.address_for_auth.c_str();
  dbauth_client_t dbauth_client = make_dbauth_client(address_for_auth)();
  AuthServer auth_server(std::move(dbauth_client));

  auth_server.set_mempool_client(std::move(mempool_client));
  AuthRpcServers rpc_servers(auth_server);
  auto [auth_db_server_port, auth_mempool_server_port] =
      rpc_servers.start_servers(db_port, mempool_port);
  CHECK(auth_db_server_port == db_port || db_port == 0);
  CHECK(auth_mempool_server_port == mempool_port || mempool_port == 0);
  info("PORT auth.db={}", auth_db_server_port);
  info("PORT auth.mempool={}", auth_mempool_server_port);

  // Re-introduce using the actual bound ports so mempool registration
  // reflects runtime-selected port values.
  IntroAuthResult updated_intro =
      mempool_client.mempoolauth_intro_auth(rpc_servers.make_auth_card(hostname))();
  if (updated_intro.outcome() != IntroAuthOutcome::kSuccess) {
    throw std::runtime_error("Failed to register auth with mempool");
  }

  auth_server.subscribe_to_auth_data_changes(
      rpc_servers.make_auth_card(hostname))();

  info("auth server initialization complete.");
  std::promise<void>().get_future().wait();
  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println(
      "usage: {} --mempool-addr <host:port> [--db-port <port>] "
      "[--mempool-port <port>]",
      argv[0]);
  exit(EXIT_FAILURE);
}

uint16_t
parse_port(const std::string &value)
{
  size_t pos = 0;
  unsigned long port = std::stoul(value, &pos, 10);
  if (pos != value.size() || port > 65535) {
    throw std::runtime_error(std::format("Invalid port: {}", value));
  }
  return static_cast<uint16_t>(port);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting auth server!");

  std::string mempool_address;
  uint16_t db_port = 0;
  uint16_t mempool_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mempool-addr" && i + 1 < argc) {
      mempool_address = argv[++i];
      continue;
    }
    if (arg == "--db-port" && i + 1 < argc) {
      db_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port = parse_port(argv[++i]);
      continue;
    }
    usage(argv);
  }

  if (mempool_address.empty()) {
    usage(argv);
  }

  return hogd::auth_init(mempool_address, db_port, mempool_port);
}
