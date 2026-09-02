// network.h - support for commonly used networking features

#pragma once

#include <bit>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

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

// Returns true when make_bind_host_port() will accept bind_ip, i.e. when it is
// a numeric address rather than a name needing DNS. Startup paths that take the
// bind address from a command line should ask this first and report a normal
// argument error: the panic above is not an exception, so a typo like
// "localhost" would otherwise escape main()'s catch as a stack trace.
inline bool
is_numeric_bind_host(std::string_view bind_ip)
{
  return x23::HostPort(std::string(bind_ip), uint16_t{0})
      .to_addr(/*dns=*/false)
      .has_value();
}

inline std::string
format_advertised_address(std::string_view bind_ip, uint16_t port)
{
  return std::string(make_bind_host_port(bind_ip, port).show());
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
