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

    {
      auto ptr = fdgen_.take();
      for (auto [fd, gen] : *ptr)
        cp_.poison(fd, gen);
    }

    destroy_wait();
  }

  std::pair<uint16_t, x23::UniqueFd> bind_server(uint16_t port)
  {
    CHECK(!bound_);
    bound_ = true;
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(port, backlog);
    if (port == 0)
      port = x23::get_port(x23::local_addr(lfd));
    port_ = port;
    return {port_, std::move(lfd)};
  }

  std::pair<uint16_t, x23::UniqueFd> bind_server(x23::HostPort bind_addr)
  {
    CHECK(!bound_);
    bound_ = true;
    std::expected<sockaddr_storage, x23::GaiError> addr =
        bind_addr.to_addr(/*dns=*/false);
    CHECK(addr.has_value());
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(*addr, backlog);
    port_ = x23::get_port(x23::local_addr(lfd));
    return {port_, std::move(lfd)};
  }

private:
  struct ImplPtrOps : x23::detail::RefOps<RpcServer *> {
    static auto get(RpcServer *p) noexcept { return &p->impl_; }
  };
  using ImplPtr = x23::RefPtr<RpcServer, ImplPtrOps>;

public:
  void start_accepting(x23::UniqueFd lfd)
  {
    CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
    listen_fd_ = lfd.get();
    x23::start_listening(cp_, lfd,
                         [self = x23::RefPtr(*this),
                          _d = x23::Defer([this] { eof(); })](auto c) {
                           c->max_message_size = self->max_message_size_;
                           self->fdgen_->insert_or_assign(c->get_fd(),
                                                          self->cp_.generation(
                                                              c->get_fd()));
                           c->template server<VersMeta>(ImplPtr(*self));
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
    auto [p, lfd] = bind_server(std::move(bind_addr));
    start_accepting(std::move(lfd));
    return p;
  }

  Impl *impl() noexcept { return &impl_; }
  uint16_t port() const noexcept { return port_; }
  RpcServer *operator->() noexcept { return this; }

private:
  Impl impl_;
  x23::MVar<std::map<int, uint64_t>> fdgen_;
  bool bound_{};
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
