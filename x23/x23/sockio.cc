
#include <system_error>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "x23/sockio.h"

namespace x23 {

const std::error_category &
gai_category()
{
  struct gai_category_impl : public std::error_category {
    const char *name() const noexcept override { return "DNS"; }
    std::string message(int err) const override { return gai_strerror(err); }
  };
  static const gai_category_impl cat;
  return (cat);
}

void
set_nodelay(BorrowedFd fd, bool on)
{
  int val = on;
  if (setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &val, sizeof(int)) == -1)
    Errno{}.raise("TCP_NODELAY");
}

void
set_nonblock(BorrowedFd fd, bool on)
{
  int n = fcntl(fd.get(), F_GETFL);
  if (n == -1)
    Errno{}.raise("F_GETFL");
  if (bool(n & O_NONBLOCK) == on)
    return;
  if (on)
    n |= O_NONBLOCK;
  else
    n &= ~O_NONBLOCK;
  if (fcntl(fd.get(), F_SETFL, n) == -1)
    Errno{}.raise("F_SETFL");
}

UniqueFd
tcplisten(std::uint16_t port, int backlog)
{
  UniqueFd lfd(socket(AF_INET6, SOCK_STREAM, 0));
  if (!lfd)
    Errno{}.raise("socket");
  if (int flags = fcntl(lfd.get(), F_GETFD);
      flags == -1 || fcntl(lfd.get(), F_SETFD, flags | FD_CLOEXEC) == -1)
    Errno{}.raise("F_SETFD");

  int opt = 0;
  if (setsockopt(lfd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) < 0)
    Errno{}.raise("IPV6_V6ONLY");
  if (port) {
    opt = 1;
    if (setsockopt(lfd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
      Errno{}.raise("SO_REUSEADDR");
  }

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(lfd.get(), reinterpret_cast<struct sockaddr *>(&addr),
           sizeof(addr)) < 0)
    Errno{}.raise("bind in6addr_any");

  if (listen(lfd.get(), backlog) < 0)
    Errno{}.raise("listen");
  return lfd;
}

UniqueFd
tcpconnect(const sockaddr_storage &ss, std::function<void(int)> prep_fd)
{
  UniqueFd fd(socket(ss.ss_family, SOCK_STREAM, 0));
  if (!fd)
    Errno{}.raise("socket({}, SOCK_STREAM, 0)", ss.ss_family);
  if (prep_fd)
    prep_fd(fd.get());
  if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sizeof(ss)) ==
      -1)
    Errno{}.raise("connect({})", HostPort(ss));
  return fd;
}

sockaddr_storage
local_addr(BorrowedFd s)
{
  sockaddr_storage ss;
  socklen_t len{sizeof(ss)};
  if (getsockname(s.get(), reinterpret_cast<sockaddr *>(&ss), &len) == -1)
    Errno{}.raise("getsockname({})", s.get());
  return ss;
}

sockaddr_storage
remote_addr(BorrowedFd s)
{
  sockaddr_storage ss;
  socklen_t len{sizeof(ss)};
  if (getpeername(s.get(), reinterpret_cast<sockaddr *>(&ss), &len) == -1)
    Errno{}.raise("getpeername({})", s.get());
  return ss;
}

std::uint16_t
get_port(const sockaddr_storage &ss)
{
  switch (ss.ss_family) {
  case AF_INET:
    return ntohs(reinterpret_cast<const sockaddr_in *>(&ss)->sin_port);
  case AF_INET6:
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&ss)->sin6_port);
  default:
    Errno(EINVAL).raise("Invalid address family {} does not have port number",
                        ss.ss_family);
  }
}

Str
show_ipaddr(const sockaddr_storage &ss)
{
  auto ntop = []<int AF>(const void *in) -> Str {
    Str ret;
    ret.resize_uninitialized(AF == AF_INET ? INET_ADDRSTRLEN
                                           : INET6_ADDRSTRLEN);
    if (!inet_ntop(AF_INET, in, ret.data(), ret.size()))
      Errno{}.raise("inet_ntop(family {})", AF);
    ret.resize(ret.find('\0'));
    return ret;
  };

  Str ret;
  switch (ss.ss_family) {
  case AF_INET6:
  {
    const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
      return ntop.operator()<AF_INET>(&sin6->sin6_addr.s6_addr[12]);
    return ntop.operator()<AF_INET6>(&sin6->sin6_addr);
  }
  case AF_INET:
  {
    const auto *sin = reinterpret_cast<const sockaddr_in *>(&ss);
    return ntop.operator()<AF_INET>(&sin->sin_addr);
  }
  default:
    Errno(EINVAL).raise("Invalid address family {} does not have IP address",
                        ss.ss_family);
  }
}

Str
show_addr(const sockaddr *sa)
{
  switch (sa->sa_family) {
  case AF_INET:
  case AF_INET6:
    return HostPort(*reinterpret_cast<const sockaddr_storage *>(sa)).show();
  case AF_UNIX:
    return "AF_UNIX";
  default:
    return uformat("sa_family {}", sa->sa_family);
  }
}

namespace detail {
Str
join_addr(std::string_view host, std::string_view port)
{
  Str ret;
  if (host.find(':') == std::string_view::npos)
    ret.append_range(host);
  else {
    ret.push_back('[');
    ret.append_range(host);
    ret.push_back(']');
  }
  ret.push_back(':');
  ret.append_range(port);
  return ret;
}
} // namespace detail

bool
HostPort::parse(std::string_view s)
{
  auto colon = s.rfind(':');
  if (colon == std::string_view::npos || colon == 0)
    return false;

  if (s[0] == '[' && s[colon - 1] == ']')
    host = Str(std::from_range, s.substr(1, colon - 2));
  else
    host = Str(std::from_range, s.substr(0, colon - 1));
  port = Str(std::from_range, s.substr(colon + 1));
  return true;
}

std::expected<sockaddr_storage, GaiError>
HostPort::to_addr(bool dns)
{
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = dns ? AI_ADDRCONFIG : AI_NUMERICHOST | AI_ADDRCONFIG;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  ManagedPtr<freeaddrinfo> ai(nullptr);

  if (int err = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai))
    return GaiError{err};

  sockaddr_storage ret{};
  if (ai->ai_addrlen > sizeof(ret))
    return GaiError{EAI_MEMORY};
  memcpy(&ret, ai->ai_addr, ai->ai_addrlen);
  return ret;
}

} // namespace x23
