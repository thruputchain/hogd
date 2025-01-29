// -*-C++-*-

#pragma once

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

// Set (on) or clear (!on) the O_NONBLOCK flag on a file descriptor
void set_nonblock(BorrowedFd fd, bool on = true);

// Listen on a TCP port on any IP address (allowing IPv4 and IPv6)
UniqueFd tcplisten(std::uint16_t port);

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
  HostPort(const sockaddr_storage &ss)
    : host(show_ipaddr(ss)), port(uformat("{}", get_port(ss)))
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

} // namespace x23
