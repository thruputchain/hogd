// -*-C++-*-

#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

#include <sys/un.h>

#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/fileio.h>
#include <x23/utility.h>
#include <x23/vec.h>

namespace x23 {

// An error category for getaddrinfo() / gai_strerror()
const std::error_category &gai_category();

// Error category for the functions getaddrinfo() and getnameinfo()
class GaiError : public ErrorBase {
public:
  GaiError(int err) noexcept : err_(err) {}
  int raw_code() const { return err_; }
  static const std::error_category &category() { return gai_category(); }
  explicit operator bool() const { return err_; }

private:
  int err_;
};

// Set or clear the TCP nodelay flag
void set_nodelay(BorrowedFd fd, bool on = true);

// Set (on) or clear (!on) the O_NONBLOCK flag on a file descriptor
void set_nonblock(BorrowedFd fd, bool on = true);

// After calling this function, closing fd (which must be a TCP
// socket) will send a RST segment instead of a FIN segment, thereby
// avoiding TIME_WAIT on the active closer.
void enable_abortive_close(BorrowedFd fd);

// Listen on a TCP port on any IP address (allowing IPv4 and IPv6).
// stcplisten does not make the socket non-blocking
UniqueFd stcplisten(std::uint16_t port, int backlog = 5);

inline UniqueFd
tcplisten(std::uint16_t port, int backlog = 5)
{
  auto fd = stcplisten(port, backlog);
  set_nonblock(fd);
  return fd;
}

constexpr socklen_t
sockaddr_len(const sockaddr_storage &ss)
{
  switch (ss.ss_family) {
  case AF_INET:
    return sizeof(sockaddr_in);
  case AF_INET6:
    return sizeof(sockaddr_in6);
  case AF_UNIX:
    static_assert(sizeof(sockaddr_un) <= sizeof(sockaddr_storage));
    return sizeof(sockaddr_un);
  default:
    return 0;
  }
}

sockaddr_storage local_addr(BorrowedFd s);
sockaddr_storage remote_addr(BorrowedFd s);

std::uint16_t get_port(const sockaddr_storage &ss);

namespace detail {
// Join a host address and port number with a colon.  If the address
// itself contains colons (because it is IPv6), then place the address
// in brackets.
Str join_addr(std::string_view host, std::string_view port);
} // namespace detail

Str show_ipaddr(const sockaddr_storage &ss);

struct HostPort {
  Str host;
  Str port;

  constexpr HostPort() noexcept = default;
  HostPort(Str h, Str p) noexcept : host(std::move(h)), port(std::move(p)) {}
  HostPort(Str h, uint16_t p)
    : host(std::move(h)), port(std::to_string(p).c_str())
  {}
  HostPort(const char *h, uint16_t p) : host(h), port(std::to_string(p).c_str())
  {}
  HostPort(const std::string &h, uint16_t p)
    : host(std::from_range, h), port(std::to_string(p).c_str())
  {}
  HostPort(const sockaddr_storage &ss)
    : host(show_ipaddr(ss)), port(uformat("{}", get_port(ss)))
  {}
  HostPort(const sockaddr *sa)
    : HostPort(*reinterpret_cast<const sockaddr_storage *>(sa))
  {}

  // Parse a joined HostPort, returns false if it fails to parse.
  bool parse(std::string_view s);

  // Join host and port with a colon (putting brackets around an IPv6
  // host address).
  Str show() const { return detail::join_addr(host, port); }

  // Convert to socket address.  If dns is true, will use domain name
  // system to resolve host names and might block.  Otherwise, if dns
  // is false, the host must be numeric.
  std::expected<sockaddr_storage, GaiError> to_addr(bool dns = false);
};

Str show_addr(const sockaddr *);
inline Str
show_addr(const is_one_of<sockaddr_storage, sockaddr_in, sockaddr_in6> auto &s)
{
  return show_addr(reinterpret_cast<const sockaddr *>(&s));
}

} // namespace x23

template<>
struct std::formatter<x23::HostPort> : std::formatter<std::string_view> {
  using super = std::formatter<std::string_view>;
  auto format(const x23::HostPort &hp, auto &ctx) const
  {
    return super::format(hp.show(), ctx);
  }
};

namespace x23 {

// Create a TCP connection.  You can bind the socket or make the file
// descriptor non-blocking in prep_fd.
UniqueFd tcpconnect(const sockaddr_storage &ss,
                    std::function<void(int)> prep_fd = nullptr);
inline UniqueFd
tcpconnect(auto host, auto port, std::function<void(int)> prep_fd = nullptr)
    requires requires { HostPort{std::move(host), std::move(port)}; }
{
  if (auto addr = HostPort{host, port}.to_addr())
    return tcpconnect(*addr, std::move(prep_fd));
  else
    throw addr.error();
}

} // namespace x23
