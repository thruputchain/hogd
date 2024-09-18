
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "x23/fileio.h"

namespace x23 {

std::expected<SeekBounds, Errno>
seek_bounds(BorrowedFd fd)
{
  off_t cur = lseek(fd.get(), 0, SEEK_CUR);
  if (cur == -1)
    return Errno{};
  off_t end = lseek(fd.get(), 0, SEEK_END);
  if (lseek(fd.get(), cur, SEEK_SET) == -1 || end == -1)
    return Errno{};
  return SeekBounds{.cur = cur, .end = end};
}

std::expected<void, Errno>
fullwrite(BorrowedFd fd, BufView buf)
{
  for (;;) {
    ssize_t n = ::write(fd.get(), buf.data(), buf.size());
    if (n <= 0)
      return Errno{};
    else if (size_t(n) == buf.size())
      return {};
    buf = buf.subspan(n);
  }
}

std::expected<void, Errno>
writefile(const std::filesystem::path &p, BufView buf, bool sync, int mode)
{
  UniqueFd fd(open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode));
  if (!fd)
    return Errno{};
  auto ret = fullwrite(fd, buf);
  if (ret) {
    if (fsync(fd.get()) == -1)
      ret = Errno{};
  }
  return ret;
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

} // namespace x23
