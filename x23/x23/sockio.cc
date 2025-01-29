
#include <netdb.h>
#include <system_error>

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
tcplisten(std::uint16_t port)
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

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(lfd.get(), reinterpret_cast<struct sockaddr *>(&addr),
           sizeof(addr)) < 0)
    Errno{}.raise("bind in6addr_any");

  if (listen(lfd.get(), 5) < 0)
    Errno{}.raise("listen");
  set_nonblock(lfd.get());
  return lfd;
}

sockaddr_storage
local_addr(BorrowedFd s)
{
  sockaddr_storage ss;
  socklen_t len{sizeof(ss)};
  if (getsockname(s.get(), reinterpret_cast<sockaddr *>(&ss), &len) == -1)
    Errno{}.raise("getsockname");
  return ss;
}

sockaddr_storage
remote_addr(BorrowedFd s)
{
  sockaddr_storage ss;
  socklen_t len{sizeof(ss)};
  if (getpeername(s.get(), reinterpret_cast<sockaddr *>(&ss), &len) == -1)
    Errno{}.raise("getsockname");
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

std::string
show_addr(const sockaddr_storage &ss)
{
  char host[NI_MAXHOST], port[NI_MAXSERV];
  if (int err = getnameinfo(reinterpret_cast<const sockaddr *>(&ss), sizeof(ss),
                            host, sizeof(host), port, sizeof(port),
                            NI_NUMERICHOST | NI_NUMERICSERV))
    throw std::system_error{err, gai_category(), "getnameinfo"};
  using sv = std::string_view;
  if (strchr(host, ':'))
    return std::format("[{}]:{}", sv(host), sv(port));
  return std::format("{}:{}", sv(host), sv(port));
}

/* work in progress...
std::expected<sockaddr_storage, GaiError>
parse_addr(std::string_view s)
{
  if (s.empty())
    return GaiError{EAI_FAMILY};

  std::string addr;
  if (s[0] == '[') {
    auto ae = s.rfind(1, ']');
  }
}
*/

} // namespace x23
