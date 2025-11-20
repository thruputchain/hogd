#pragma once

#include <format>
#include <memory>
#include <string>
#include <utility>

#include "auth_rpc.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/mock_dbauth_rpc.h"
#include "hogd/base/types.h"
#include "hogd/version.h"
#include "sched_db.hh"

namespace hogd {

// Test framework for AuthServer that sets up a mock DB RPC server
// and creates an AuthServer connected to it.
class AuthServerTestFramework {
public:
  AuthServerTestFramework()
  {
    // Start the mock DB server on a random port
    uint16_t port = mock_db_server_.start_server(0);
    server_port_ = port;
    // Create ShardCard with the auth address
    std::string auth_address = std::format("localhost:{}", port);
    ShardCard shard_card{.protocol = version::kHogdProtocol,
                         .upid = {},
                         .block_height = 0,
                         .address_for_exec = "",
                         .address_for_sched = "",
                         .address_for_auth = auth_address.c_str()};
    // Create IntroAuthResult
    IntroAuthResult intro_result;
    intro_result.shards.push_back(shard_card);

    // Create AuthServer
    auth_server_ = std::make_unique<AuthServer>(std::move(intro_result));
  }

  ~AuthServerTestFramework() { mock_db_server_.shut_down(); }

  AuthServer &auth_server() { return *auth_server_; }
  MockDBAuthRpc &mock_db() { return *mock_db_server_.impl(); }
  uint16_t server_port() const { return server_port_; }

private:
  MockDBAuthServer mock_db_server_;
  uint16_t server_port_;
  std::unique_ptr<AuthServer> auth_server_;
};

} // namespace hogd
