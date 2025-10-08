#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <tuple>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "x23/cleanup.h"
#include "x23/x23/event.h"
#include "x23/x23/sockio.h"
#include "x23/x23/threadpool.h"
#include "x23/x23/rpc.h" // IWYU pragma: keep

namespace hogd {

template<typename RpcImplementation> class RpcServer {
public:
  template<typename... Args>
  explicit RpcServer(Args &&...args) : rpc_(std::forward<Args>(args)...)
  {}

  // Starts the server on the specified port.
  uint16_t start_server(uint16_t port)
  {
    std::tie(port_, listen_fd_) =
        tcp_rpc_server(cp_, port, [this](auto c) { c->server(&rpc_); });
    return port_;
  }

  void shut_down()
  {
    cp_.poison(listen_fd_);
    rpc_.shut_down();
  }

  void await_shutdown_request() { rpc_.await_shutdown_request(); }

  // Returns the port that the server is listening on.
  uint16_t port() const { return port_; }

  RpcImplementation &get_impl() { return rpc_; }
  x23::CoroPoll &cp() { return cp_; }

private:
  x23::BorrowedFd listen_fd_{-1};

  template<typename T> requires std::is_trivially_copyable_v<T>
  ErrnoStatus<void> sys_getsockname(int sockfd, T &val)
  {
    socklen_t len = sizeof(val); // NOLINT(misc-include-cleaner)
    return make_errno_status<void>(
        getsockname(sockfd, reinterpret_cast<sockaddr *>(&val), &len));
  }

  uint16_t get_port(int fd)
  {
    sockaddr_in saddr;
    // retrieve the port number if it was assigned automatically.
    ErrnoStatus<void> ret = sys_getsockname(fd, saddr);
    if (!ret)
      throw std::runtime_error("Failed to get socket name");
    return ntohs(saddr.sin_port);
  }

  RpcImplementation rpc_;

  x23::ThreadPool pool_;
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);
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
