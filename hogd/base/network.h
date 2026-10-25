// network.h - support for commonly used networking features

#pragma once

#include <bit>
#include <cstdlib>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <sys/socket.h>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
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

// Resolves a peer's [host_port], using DNS if the host is a name. On failure,
// throws the x23::GaiError as a std::system_error rather than panicking, so
// that if nothing catches it the process reports the bad address and exits
// instead of aborting (see hogd/base/config_failure.h).
inline sockaddr_storage
resolve_host_port(const x23::HostPort &host_port)
{
  std::expected<sockaddr_storage, x23::GaiError> addr =
      host_port.to_addr(/*dns=*/true);
  if (!addr)
    addr.error().raise("resolve {}", host_port);
  return *addr;
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
