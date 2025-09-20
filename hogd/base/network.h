// network.h - support for commonly used networking features

#pragma once

#include <bit>

#include "hogd/base/compiler.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"

namespace hogd {

inline int
get_hostname(std::string &hostname)
{
  char hostname_raw[HOST_NAME_MAX + 1];
  int ret = gethostname(hostname_raw, sizeof(hostname_raw));
  if (!ret) {
    hostname = std::string(hostname_raw);
  }
  return ret;
}

} // namespace hogd
