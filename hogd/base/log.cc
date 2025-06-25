#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
// NOLINTNEXTLINE(misc-include-cleaner)
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <syncstream>
#include <vector>

#include "hogd/config.h"

#if HAVE_CXX_STACKTRACE
#include <stacktrace>
#endif // HAVE_CXX_STACKTRACE

#include "hogd/base/ansi_color.h"
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

[[noreturn]] void
do_panic(std::string_view fmt, const std::source_location &loc,
         std::format_args &&args)
{
  std::string buf;
  std::vformat_to(std::back_inserter(buf), fmt, args);

#if HAVE_CXX_STACKTRACE
  auto trace = std::stacktrace::current();
  std::format_to(std::back_inserter(buf), "\n{}",
                 ansi_color(Style::kNormal, Color::kGreen));
  for (const auto &entry : trace | std::views::drop(2)) {
    std::format_to(std::back_inserter(buf), "{}\n", entry);
  }
  std::format_to(std::back_inserter(buf), "{}", ansi_reset());
#endif // HAVE_CXX_STACKTRACE

  // If you are segfaulting on the line below, it is likely because you have not
  // called [log_init()], so [logger] is a [nullptr].
  logger->vlog(LogLevel::kCritical, "{}", loc, std::make_format_args(buf));
  detail::logger->flush();
  // TODO(amb): do more to cleanup safely (e.g., flush to disk?)
  std::terminate();
}

} // namespace detail

void
ConsoleLogger::vlog(LogLevel level, std::string_view fmt,
                    const std::source_location &loc, std::format_args args)
{
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
  std::format_to(std::back_inserter(buf), "{}{}{}{:%m%d %H:%M:%OS} {}:{}]{} ",
                 ansi_color(Style::kBold), log_level_to_char(level),
                 ansi_color(Style::kNormal, Color::kBlue), now, filename,
                 loc.line(), ansi_reset());
  std::vformat_to(std::back_inserter(buf), fmt, args);
  std::format_to(std::back_inserter(buf), "\n");

  // write to stderr
  // todo(amb): maybe write to the fd directly.
  std::osyncstream(std::cerr) << buf;
}

void
ConsoleLogger::flush()
{
  // Note: cerr doesn't buffer by default, but flush just in case it was
  // configured differently.
  std::cerr.flush();
}

} // namespace hogd
