#pragma once

#include <cstdio>
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
    // The fallback must not allocate or throw: log_error is noexcept and is
    // called from other noexcept FFI helpers, so a second diagnostic failure
    // here would escape and terminate the process (issue #2444). error()
    // formats and allocates, so report straight to stderr instead. e.what() is
    // noexcept and returns a null-terminated string, so fputs is safe.
    std::fputs("test_ffi: failed to log error: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputc('\n', stderr);
  }
}

} // namespace hogd
