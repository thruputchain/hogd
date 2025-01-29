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

// Convert the sockaddr to a string containing Address:Port, or
// [Address]:Port for IPv6 addresses that contain : characters.
std::string show_addr(const sockaddr_storage &ss);

std::expected<sockaddr_storage, GaiError> parse_addr(std::string_view s);

} // namespace x23
