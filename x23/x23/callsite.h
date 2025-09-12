// -*- C++ -*-

#pragma once

#include <format>
#include <source_location>
#include <string>
#include <string_view>

#include <x23/config.h>

#if HAVE_STD_STACKTRACE
#include <stacktrace>
#endif // HAVE_STD_STACKTRACE

namespace x23 {

using std::source_location;

// Convert a source_location to a string in "filename:line" format.
inline std::string
to_string(const source_location &s)
{
  return std::format("{}:{}", s.file_name(), s.line());
}

#if HAVE_STD_STACKTRACE

// Get the calling location by walking the stack trace and finding the
// first frame that's not from this file or the specified
// ignore_file. Returns a string prefixed with '*' to indicate it came
// from stack trace analysis.  Falls back to the provided
// source_location if no suitable frame is found.
inline std::string
get_callsite(std::string_view ignore_file,
             source_location fallback = source_location::current())
{
  static const std::string_view this_file(__FILE__);

  auto bt = std::stacktrace::current();
  for (const auto &e : bt)
    if (e.source_line())
      if (std::string sf = e.source_file();
          sf != this_file &&
          (ignore_file.empty() || !sf.ends_with(ignore_file)))
        return std::format("*{}:{}", sf, e.source_line());
  return to_string(fallback);
}

#else // !HAVE_STD_STACKTRACE

inline std::string
get_callsite(std::string_view ignore_file,
             source_location sloc = source_location::current())
{
  return to_string(sloc);
}

#endif // !HAVE_STD_STACKTRACE

} // namespace x23
