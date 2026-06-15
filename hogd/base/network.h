// network.h - support for commonly used networking features

#pragma once

#include <bit>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

#include "hogd/base/compiler.h"
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

inline x23::HostPort
make_bind_host_port(std::string_view bind_ip, uint16_t port)
{
  x23::HostPort bind_addr(std::string(bind_ip), port);
  if (!bind_addr.to_addr(/*dns=*/false).has_value()) {
    throw std::runtime_error(std::format("Invalid bind IP: {}", bind_ip));
  }
  return bind_addr;
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
