#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <tuple>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/cleanup.h"
#include "x23/x23/event.h"
#include "x23/x23/rpc.h" // IWYU pragma: keep
#include "x23/x23/sockio.h"

namespace hogd {

template<typename RpcImplementation> class RpcServer {
public:
  template<typename... Args>
  explicit RpcServer(x23::CoroPoll &cp, Args &&...args)
    : cp_(cp), impl_(std::forward<Args>(args)...)
  {}

  template<typename... Args>
  explicit RpcServer(Args &&...args)
    : RpcServer(default_coropoll(), std::forward<Args>(args)...)
  {}

  // Starts the server on the specified port.
  uint16_t start_server(uint16_t port)
  {
    std::tie(port_, listen_fd_) =
        tcp_rpc_server(cp_, port, [this](auto c) { c->server(&impl_); });
    return port_;
  }

  void shut_down()
  {
    cp_.poison(listen_fd_);
    impl_.shut_down();
  }

  void await_shutdown_request() { impl_.await_shutdown_request(); }

  // Returns the port that the server is listening on.
  uint16_t port() const { return port_; }

  x23::CoroPoll &cp_;
  RpcImplementation impl_;

  ~RpcServer() { cp_.reset(listen_fd_); }

private:
  x23::BorrowedFd listen_fd_{-1};
  uint16_t port_ = 0;
};

inline sockaddr_storage
host_port_to_sockaddr(const std::string &host, uint16_t port)
{
  x23::HostPort helper(host, port);
  std::expected<sockaddr_storage, x23::GaiError> addr =
      helper.to_addr(/*dns=*/true);
  CHECK(addr.has_value());
  return *addr;
}

inline sockaddr_storage
server_address_to_sockaddr(std::string_view server_address)
{
  x23::HostPort helper;
  CHECK(helper.parse(server_address));
  std::expected<sockaddr_storage, x23::GaiError> addr =
      helper.to_addr(/*dns=*/true);
  CHECK(addr.has_value());
  return *addr;
}

} // namespace hogd
