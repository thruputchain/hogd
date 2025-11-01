#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <tuple>
#include <type_traits>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/cleanup.h"
#include "x23/x23/event.h"
#include "x23/x23/rpc.h" // IWYU pragma: keep
#include "x23/x23/sockio.h"

namespace hogd {

template<typename IMPL, typename VERSMETA = IMPL::rpc_service>
requires x23::implements_async_rpc<IMPL *, VERSMETA>
class RpcSvcHolder : public x23::RefCount {
public:
  using Impl = IMPL;
  using VersMeta = VERSMETA;
  using Ptr = x23::RefPtr<RpcSvcHolder>;

  static constexpr bool kAllowRefPtrConversion = true;

  x23::CoroPoll &cp_;
  Impl impl_;

  template<typename... Args> requires std::constructible_from<Impl, Args...>
  static RpcSvcHolder &make(x23::CoroPoll &cp, Args &&...args)
  {
    return *new RpcSvcHolder(cp, std::forward<Args>(args)...);
  }

  uint16_t start_server(uint16_t portno)
  {
    // If this fails you started the same server twice
    assert(listen_fd_ == x23::kInvalidFd);
    auto [port, lfd] =
        tcp_rpc_server(cp_, portno,
                       [this, _d = x23::Defer([this] { eof(); })](auto c) {
                         c->max_message_size = max_message_size_;
                         c->template server<VersMeta>(&impl_);
                       });
    listen_fd_ = lfd.get();
    return port_ = port;
  }

  uint16_t port() const { return port_; }
  size_t max_message_size_ = x23::default_max_message_size;

private:
  std::atomic_int listen_fd_{x23::kInvalidFd};
  uint16_t port_{};

  template<typename... Args>
  RpcSvcHolder(x23::CoroPoll &cp, Args &&...args)
    : cp_(cp), impl_(std::forward<Args>(args)...)
  {}

  ~RpcSvcHolder() = default;

  void finalize() noexcept
  {
    if (int fd = listen_fd_.exchange(x23::kInvalidFd); fd != x23::kInvalidFd)
      cp_.poison(fd);
    else
      delete this;
  }

  void eof() noexcept
  {
    Ptr _r(*this);
    listen_fd_ = -1;
  }

  friend class x23::RefCount;
};

template<typename IMPL, typename VERSMETA = IMPL::rpc_service>
requires x23::implements_async_rpc<IMPL *, VERSMETA>
struct RpcServer : RpcSvcHolder<IMPL, VERSMETA>::Ptr {
  template<typename... Args> requires std::constructible_from<IMPL, Args...>
  RpcServer(x23::CoroPoll &cp, Args &&...args)
    : RpcSvcHolder<IMPL, VERSMETA>::Ptr(
          RpcSvcHolder<IMPL, VERSMETA>::make(cp, std::forward<Args>(args)...))
  {}
  template<typename... Args> requires std::constructible_from<IMPL, Args...>
  RpcServer(Args &&...args)
    : RpcSvcHolder<IMPL, VERSMETA>::Ptr(
          RpcSvcHolder<IMPL, VERSMETA>::make(default_coropoll(),
                                             std::forward<Args>(args)...))
  {}
  uint16_t start_server(uint16_t portno)
  {
    return (*this)->start_server(portno);
  }
  uint16_t port() const { return (*this)->port(); }
};
template<typename I>
requires std::constructible_from<std::remove_cvref_t<I>, I &&>
RpcServer(x23::CoroPoll &, I &&) -> RpcServer<std::remove_cvref_t<I>>;

template<typename I>
requires std::constructible_from<std::remove_cvref_t<I>, I &&>
RpcServer(I &&) -> RpcServer<std::remove_cvref_t<I>>;

#if 0
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
    std::tie(port_, listen_fd_) = tcp_rpc_server(cp_, port, [this](auto c) {
      c->max_message_size = max_message_size_;
      c->server(&impl_);
    });
    return port_;
  }

  void shut_down()
  {
    cp_.poison(listen_fd_);
    impl_.shut_down();
  }

  // Returns the port that the server is listening on.
  uint16_t port() const { return port_; }

  x23::CoroPoll &cp_;
  RpcImplementation impl_;
  size_t max_message_size_ = x23::default_max_message_size;

  ~RpcServer() { cp_.reset(listen_fd_); }

private:
  x23::BorrowedFd listen_fd_{-1};
  uint16_t port_ = 0;
};
#endif

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
