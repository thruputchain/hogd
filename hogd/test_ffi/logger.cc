#include "hogd/test_ffi/logger.h"

#include <chrono>
#include <format>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <syncstream>
#include <vector>

#include "hogd/base/ansi_color.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"

namespace hogd {

void
ThreadsafeLogger::vlog(LogLevel level, std::string_view fmt,
                       const std::source_location &loc, std::format_args args)
{
  // copied from ConsoleLogger::vlog
  // check if the severity is high enough
  if (level > max_level_)
    return;

  // record the current timestamp
  const auto now = std::chrono::system_clock::now();

  // extract the filename
  std::vector<std::string_view> tokens = split(loc.file_name(), '/');
  std::string_view filename = tokens.back();

  // generate the log message
  std::string buf;
  const UtcTime utc(now);
  std::format_to(std::back_inserter(buf),
                 "{}{}{}[{:02}{:02} {:02}:{:02}:{:02} {}:{}]{} ",
                 ansi_color(Style::kBold), log_level_to_char(level),
                 ansi_color(Style::kNormal, Color::kBlue), utc.month, utc.day,
                 utc.hours, utc.minutes, utc.seconds, filename, loc.line(),
                 ansi_reset());
  std::vformat_to(std::back_inserter(buf), fmt, args);
  std::format_to(std::back_inserter(buf), "\n");

  // write to stderr
  // todo(amb): maybe write to the fd directly.
  std::osyncstream(std::cerr) << buf;
}

void
ThreadsafeLogger::flush()
{
  // copied from ConsoleLogger::flush
  // Note: cerr doesn't buffer by default, but flush just in case it was
  // configured differently.
  std::cerr.flush();
}

} // namespace hogd
