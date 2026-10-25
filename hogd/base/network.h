// network.h - support for commonly used networking features

#pragma once

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "x23/cleanup.h"
#include "x23/err.h"
#include "x23/sockio.h"

#ifdef __APPLE__
#include <sys/param.h>
#include <unistd.h> // for gethostname
#define HOST_NAME_MAX MAXHOSTNAMELEN
#else
#include <climits> // for HOST_NAME_MAX
#endif

namespace hogd {

// The bind IP always comes from startup configuration, so a bad one is fatal:
// panic rather than unwinding an exception out through main().
inline x23::HostPort
make_bind_host_port(std::string_view bind_ip, uint16_t port)
{
  x23::HostPort bind_addr(std::string(bind_ip), port);
  if (!bind_addr.to_addr(/*dns=*/false).has_value()) {
    panic("Invalid bind IP: {}", bind_ip);
  }
  return bind_addr;
}

inline std::string
format_advertised_address(std::string_view bind_ip, uint16_t port)
{
  return std::string(make_bind_host_port(bind_ip, port).show());
}

// Resolves a peer's [host_port], using DNS if the host is a name. A peer
// address that does not resolve is a configuration problem, not a bug, so
// rather than panic (and dump core) this logs why and exits.
inline sockaddr_storage
resolve_or_exit(const x23::HostPort &host_port)
{
  std::expected<sockaddr_storage, x23::GaiError> addr =
      host_port.to_addr(/*dns=*/true);
  if (!addr) {
    error("cannot resolve {}: {}. Check the address, or give a numeric IP.",
          host_port, addr.error());
    detail::logger->flush();
    // _Exit, like panic, runs no destructors or atexit handlers: other
    // threads may already be running.
    std::_Exit(EXIT_FAILURE);
  }
  return *addr;
}

// A daemon's listen addresses come from its command line, so failing to listen
// on one (most often because the port is already in use) is likewise a
// configuration problem: log why and exit rather than abort.
[[noreturn]] inline void
exit_cannot_listen(const x23::HostPort &bind_addr, const x23::Errno &err)
{
  error("cannot listen on {}: {}{}", bind_addr, err,
        err == EADDRINUSE ? ". Is another instance already running, or is "
                            "another service using that port?"
                          : "");
  detail::logger->flush();
  std::_Exit(EXIT_FAILURE);
}

// Starts a daemon's [server] listening on [bind_ip]:[port], exiting if it
// cannot.
template<typename Server>
uint16_t
start_server_or_exit(Server &server, std::string_view bind_ip, uint16_t port)
{
  const x23::HostPort bind_addr = make_bind_host_port(bind_ip, port);
  std::expected<uint16_t, x23::Errno> actual =
      server.try_start_server(bind_addr);
  if (!actual)
    exit_cannot_listen(bind_addr, actual.error());
  return *actual;
}

// Like start_server_or_exit, for a server that must learn its port before it
// starts accepting.
template<typename Server>
std::pair<uint16_t, x23::UniqueFd>
bind_server_or_exit(Server &server, std::string_view bind_ip, uint16_t port)
{
  const x23::HostPort bind_addr = make_bind_host_port(bind_ip, port);
  auto bound = server.try_bind_server(bind_addr);
  if (!bound)
    exit_cannot_listen(bind_addr, bound.error());
  return std::move(*bound);
}

inline int
get_hostname(std::string &hostname)
{
  if (auto *hostname_raw = getenv("HOSTNAME"); hostname_raw != nullptr) {
    hostname = hostname_raw;
    return 0;
  }

  char hostname_raw[HOST_NAME_MAX + 1];
  int ret = gethostname(hostname_raw, sizeof(hostname_raw));
  if (!ret) {
    hostname = std::string(hostname_raw);
  }
  return ret;
}

} // namespace hogd
