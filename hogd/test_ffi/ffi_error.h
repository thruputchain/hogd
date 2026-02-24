#pragma once

#include <exception>
#include <format>

#include "hogd/base/log.h"

namespace hogd {

// error("message {}", e.what()); can throw an allocation failure;
// This function works around that issue.
inline void
log_error(std::format_string<const char *> message,
          std::exception const &e) noexcept
{
  try {
    error(message, e.what());
  } catch (...) {
    error("Failed to print error message!");
  }
}

} // namespace hogd
