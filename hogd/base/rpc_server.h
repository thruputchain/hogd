#pragma once

#include <atomic>
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
#include "x23/refcnt.h"
#include "x23/x23/event.h"
#include "x23/x23/rpc.h" // IWYU pragma: keep
#include "x23/x23/sockio.h"

namespace hogd {

template<typename IMPL, typename VERSMETA = IMPL::rpc_service>
requires x23::implements_async_rpc<IMPL *, VERSMETA>
class RpcServer {
public:
  using Impl = IMPL;
  using VersMeta = VERSMETA;

  x23::CoroPoll &cp_;
  size_t max_message_size_ = x23::default_max_message_size;

  template<typename... Args> requires std::constructible_from<Impl, Args...>
  RpcServer(x23::CoroPoll &cp, Args &&...args)
    : cp_(cp), impl_(std::forward<Args>(args)...)
  {}
  template<typename... Args> requires std::constructible_from<Impl, Args...>
  RpcServer(Args &&...args)
    : cp_(default_coropoll()), impl_(std::forward<Args>(args)...)
  {}
  ~RpcServer() { shut_down(); }

  uint16_t start_server(uint16_t port)
  {
    // If this fails you started the same server twice
    assert(listen_fd_ == x23::kInvalidFd);
    // NOLINTBEGIN(readability-identifier-naming)
    auto [prt, lfd] =
        tcp_rpc_server(cp_, port,
                       [this, _d = x23::Defer([this] { eof(); })](auto c) {
                         c->max_message_size = max_message_size_;
                         new Conn{*this, c};
                       });
    // NOLINTEND(readability-identifier-naming)
    listen_fd_ = lfd.get();
    return port_ = prt;
  }

  Impl *impl() noexcept { return &impl_; }
  uint16_t port() const noexcept { return port_; }
  RpcServer *operator->() noexcept { return this; }

  void shut_down()
  {
    if (int lfd = listen_fd_; lfd != x23::kInvalidFd) {
      cp_.poison(lfd);
      listen_fd_.wait(lfd);
    }

    auto ptr = cs_.take();
    if (Conn *c = ptr->connlist)
      do {
        c->rpcstream_->fail(false);
        c = c->next_;
      } while (c);
    else
      all_connections_destroyed_ = true;
    ptr.reset();
    all_connections_destroyed_.wait(false);
  }

private:
  struct Conn {
    RpcServer &s_;
    Conn *next_;
    Conn **prev_;
    x23::detail::RpcStreamImpl *rpcstream_;

    struct Ptr {
      Conn *c_;
      explicit Ptr(Conn *c) : c_(c) {}
      Ptr(Ptr &&other) noexcept : c_(std::exchange(other.c_, nullptr)) {}
      ~Ptr() { delete c_; }
      Impl &operator*() const noexcept { return c_->s_.impl_; }
      Impl *operator->() const noexcept { return &c_->s_.impl_; }
    };

    // NOLINTBEGIN(bugprone-assignment-in-if-condition)
    Conn(RpcServer &s, auto &c) : s_(s), rpcstream_(c.get())
    {
      auto ptr = s_.cs_.take();
      if ((next_ = ptr->connlist))
        next_->prev_ = &next_;
      prev_ = &ptr->connlist;
      *prev_ = this;
      static_assert(x23::implements_async_rpc<Ptr, VersMeta>);
      c->template server<VersMeta>(Ptr(this));
    }
    ~Conn()
    {
      auto ptr = s_.cs_.take();
      if ((*prev_ = next_))
        next_->prev_ = prev_;
      if (!ptr->connlist && s_->listen_fd_ == x23::kInvalidFd) {
        s_.all_connections_destroyed_ = true;
        s_.all_connections_destroyed_.notify_all();
      }
    }
    // NOLINTEND(bugprone-assignment-in-if-condition)
  };
  struct ConnState {
    Conn *connlist{};
  };

  Impl impl_;
  x23::MVar<ConnState> cs_;
  std::atomic_bool all_connections_destroyed_;
  std::atomic_int listen_fd_{x23::kInvalidFd};
  uint16_t port_{};

  void eof() noexcept
  {
    listen_fd_ = x23::kInvalidFd;
    listen_fd_.notify_all();
  }
};

template<typename I>
requires std::constructible_from<std::remove_cvref_t<I>, I &&>
RpcServer(x23::CoroPoll &, I &&) -> RpcServer<std::remove_cvref_t<I>>;

template<typename I>
requires std::constructible_from<std::remove_cvref_t<I>, I &&>
RpcServer(I &&) -> RpcServer<std::remove_cvref_t<I>>;

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
