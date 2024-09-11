#include <chrono>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/string.h"

namespace hogd {

namespace {

// log_level_to_char converts a log severity to a single character identifier
constexpr char
log_level_to_char(LogLevel level)
{
  using enum LogLevel;
  switch (level) {
  case kCritical:
    return 'C';
  case kError:
    return 'E';
  case kWarn:
    return 'W';
  case kInfo:
    return 'I';
  case kDebug:
    return 'D';
  case kTrace:
    return 'T';
  default:
    return 'U';
  }
}

} // namespace

namespace detail {

// logger is the logging implementation currently in use.
std::unique_ptr<Logger> logger;

} // namespace detail

void
ConsoleLogger::vlog(LogLevel level, std::string_view fmt,
                    const std::source_location &loc, std::format_args &&args)
{
  // check if the severity is high enough
  if (level > max_level_)
    return;

  // calculate the current timestamp
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(now - start_ts_);
  float sec = static_cast<float>(elapsed.count()) / 1000000;

  // extract the filename
  std::vector<std::string_view> tokens = split(loc.file_name(), '/');
  std::string_view filename = tokens.back();

  // generate the log message
  std::string buf;
  std::format_to(std::back_inserter(buf), "{} {:<8.6f} {}:{}] ",
                 log_level_to_char(level), sec, filename, loc.line());
  std::vformat_to(std::back_inserter(buf), fmt, args);
  std::format_to(std::back_inserter(buf), "\n");

  // write to stderr
  // todo(amb): maybe write to the fd directly.
  std::cerr << buf;
}

void
ConsoleLogger::flush()
{
  // Note: cerr doesn't buffer by default, but flush just in case it was
  // configured differently.
  std::cerr.flush();
}

} // namespace hogd
