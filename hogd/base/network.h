// network.h - support for commonly used networking features

#pragma once

#include <bit>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
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

// A listen or peer address that cannot be used comes from the configuration,
// so it is the operator's problem rather than a bug: log why and exit instead
// of panicking and dumping core. _Exit, like panic, runs no destructors or
// atexit handlers, since other threads may already be running.
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

[[noreturn]] inline void
exit_cannot_resolve(const x23::HostPort &host_port, const x23::GaiError &err)
{
  error("cannot resolve {}: {}. Check the address, or give a numeric IP.",
        host_port, err);
  detail::logger->flush();
  std::_Exit(EXIT_FAILURE);
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
