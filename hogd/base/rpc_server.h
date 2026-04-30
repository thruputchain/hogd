#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <type_traits>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/cleanup.h"
#include "x23/mvar.h"
#include "x23/refcnt.h"
#include "x23/x23/event.h"
#include "x23/x23/rpc.h" // IWYU pragma: keep
#include "x23/x23/sockio.h"

namespace hogd {

template<typename IMPL, typename VERSMETA = IMPL::rpc_service>
requires x23::implements_async_rpc<IMPL *, VERSMETA>
class RpcServer : public x23::DestroyInhibit {
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

  ~RpcServer()
  {
    if (int lfd = listen_fd_; lfd != x23::kInvalidFd) {
      cp_.poison(lfd);
      listen_fd_.wait(lfd);
    }

    for (;;) {
      auto ptr = cs_.take();
      Conn *c = ptr->connlist;
      if (!c)
        break;
      x23::RefPtr s(*c->rpcstream_);
      ptr.reset();
      s->fail();
    }
    destroy_wait();
  }

  std::pair<uint16_t, x23::UniqueFd> bind_server(uint16_t port)
  {
    CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(port, backlog);
    listen_fd_ = lfd.get();
    if (port == 0)
      port = x23::get_port(x23::local_addr(lfd));
    port_ = port;
    return {port_, std::move(lfd)};
  }

  std::pair<uint16_t, x23::UniqueFd> bind_server(x23::HostPort bind_addr)
  {
    CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
    std::expected<sockaddr_storage, x23::GaiError> addr =
        bind_addr.to_addr(/*dns=*/false);
    CHECK(addr.has_value());
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(*addr, backlog);
    listen_fd_ = lfd.get();
    port_ = x23::get_port(x23::local_addr(lfd));
    return {port_, std::move(lfd)};
  }

  void start_accepting(x23::UniqueFd lfd)
  {
    x23::start_listening(cp_, lfd,
                         [self = x23::RefPtr(*this),
                          _d = x23::Defer([this] { eof(); })](auto c) {
                           c->max_message_size = self->max_message_size_;
                           new Conn{*self, c};
                         });
  }

  uint16_t start_server(uint16_t port)
  {
    auto [p, lfd] = bind_server(port);
    start_accepting(std::move(lfd));
    return p;
  }

  uint16_t start_server(x23::HostPort bind_addr)
  {
    auto [p, lfd] = bind_server(bind_addr);
    start_accepting(std::move(lfd));
    return p;
  }

  Impl *impl() noexcept { return &impl_; }
  uint16_t port() const noexcept { return port_; }
  RpcServer *operator->() noexcept { return this; }

private:
  struct Conn {
    x23::RefPtr<RpcServer> s_;
    Conn *next_;
    Conn **prev_;
    x23::RefPtr<x23::detail::RpcStreamImpl> rpcstream_;

    struct Ptr {
      Conn *c_;
      explicit Ptr(Conn *c) : c_(c) {}
      Ptr(Ptr &&other) noexcept : c_(std::exchange(other.c_, nullptr)) {}
      ~Ptr() { delete c_; }
      Impl &operator*() const noexcept { return c_->s_->impl_; }
      Impl *operator->() const noexcept { return &c_->s_->impl_; }
    };

    // NOLINTBEGIN(bugprone-assignment-in-if-condition,bugprone-exception-escape)
    // noexcept because it's UB to delete objects before constructors
    // return.  If c->server throws it could delete this, so at least
    // crash close to the bug.
    Conn(RpcServer &s, auto &c) noexcept : s_(s), rpcstream_(*c)
    {
      auto ptr = s_->cs_.take();
      if ((next_ = ptr->connlist))
        next_->prev_ = &next_;
      prev_ = &ptr->connlist;
      *prev_ = this;
      static_assert(x23::implements_async_rpc<Ptr, VersMeta>);
      c->template server<VersMeta>(Ptr(this));
    }
    ~Conn()
    {
      auto ptr = s_->cs_.take();
      if ((*prev_ = next_))
        next_->prev_ = prev_;
      ptr.reset();
    }
    // NOLINTEND(bugprone-assignment-in-if-condition,bugprone-exception-escape)
  };
  struct ConnState {
    Conn *connlist{};
  };

  Impl impl_;
  x23::MVar<ConnState> cs_;
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
