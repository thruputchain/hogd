#pragma once

#include <format>
#include <memory>
#include <string>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/mock_dbauth_rpc.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/x23.h"

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
    std::string address_for_auth = std::format("localhost:{}", port);
    dbauth_client_t dbauth_client = make_dbauth_client(address_for_auth)();

    // Create AuthServer
    auth_server_ = std::make_unique<AuthServer>(std::move(dbauth_client), 16);
  }

  AuthServer &auth_server() { return *auth_server_; }
  MockDBAuthRpc &mock_db() { return *mock_db_server_.impl(); }

private:
  MockDBAuthServer mock_db_server_;
  uint16_t server_port_;
  std::unique_ptr<AuthServer> auth_server_;
};

constexpr ContractAddress
make_test_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}

inline PublicKeyConfigOpt
make_test_pk_config(uint8_t id)
{
  PublicKey key(CryptographicScheme::Ed25519);
  key.ed25519().fill(id);
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = key, .max_weight = 100});
}

inline AddressAuthData
make_test_auth_data(uint8_t id)
{
  AddressAuthData auth_data{};
  CHECK(id < kMaxSigners);
  auth_data.public_keys[id] = make_test_pk_config(id);
  auth_data.last_written_height = 0;
  return auth_data;
}

} // namespace hogd
