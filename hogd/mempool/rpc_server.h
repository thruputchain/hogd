#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <thread>

#include "hogd/base/status.h"

namespace hogd {

template<typename RpcImplementation> class RpcServer {
public:
  template<typename... Args>
  explicit RpcServer(Args &&...args) : rpc_(std::forward<Args>(args)...)
  {}

  // Starts the server on the specified port.
  uint16_t start_server(uint16_t port)
  {
    auto lfd = x23::tcplisten(port);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    std::lock_guard lock(mtx_);
    port_ = get_port(lfd.get());
    cv_.notify_all();

    [](x23::CoroPoll &cp, RpcImplementation &server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c)->server(&server);
      }
    }(cp_, rpc_, std::move(lfd));

    return port_.value();
  }

  // Returns the port that the server is listening on.
  uint16_t port()
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this] { return port_.has_value(); });
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return port_.value();
  }

  RpcImplementation &get_impl() { return rpc_; }

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
