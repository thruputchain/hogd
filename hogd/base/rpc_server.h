#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "x23/x23/event.h"
#include "x23/x23/rpc.h"
#include "x23/x23/sockio.h"
#include "x23/x23/threadpool.h"

namespace hogd {

template<typename RpcImplementation> class RpcServer {
public:
  template<typename... Args>
  explicit RpcServer(Args &&...args) : rpc_(std::forward<Args>(args)...)
  {}

  // Starts the server on the specified port.
  uint16_t start_server(uint16_t port)
  {
    x23::DestroyInhibit di;
    port = tcp_rpc_server(cp_, port, [this, di = x23::RefPtr{di}](auto c) {
      // x23::rpc_stream_logged(cp, *c)->server(&rpc_);
      c->server(&rpc_, typename RpcImplementation::rpc_service{});
    });
    // Use [di] to wait for the RPC server to start before calling
    // [socketpair()] below.
    di.destroy_wait();

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
      x23::Errno{}.raise("socketpair");
    }
    x23::rpc_stream_logged(cp_, fds[1])
        ->server<typename RpcImplementation::rpc_service>(&rpc_);

    std::lock_guard lock(mtx_);
    port_ = port;
    cv_.notify_all();

    return port_.value();
  }

  void shut_down() { rpc_.shut_down(); }

  void await_shutdown_request() { rpc_.await_shutdown_request(); }

  // Returns the port that the server is listening on.
  uint16_t port()
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this] { return port_.has_value(); });
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return port_.value();
  }

  RpcImplementation &get_impl() { return rpc_; }
  x23::CoroPoll &cp() { return cp_; }

private:
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

  // Protects [port_] during initialization.
  std::mutex mtx_;
  std::condition_variable cv_;
  std::optional<uint16_t> port_;
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
