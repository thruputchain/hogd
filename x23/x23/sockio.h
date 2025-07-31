// -*-C++-*-

#pragma once

#include <string_view>

#include <x23/err.h>
#include <x23/fileio.h>

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

// Create a TCP connection.  You can make bind the socket or make the
// file descriptor non-blocking in prep_fd.
UniqueFd tcpconnect(const sockaddr_storage &ss,
		    std::function<void(int)> prep_fd = nullptr);

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
  HostPort(Str h, Str p) : host(std::move(h)), port(std::move(p)) {}
  HostPort(Str h, uint16_t p)
    : host(std::move(h)), port(std::to_string(p).c_str())
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
