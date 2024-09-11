// log.h - logging infrastructure

#pragma once

#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <source_location>

#include "hogd/base/compiler.h"

namespace hogd {

// LogLevel indicates the severity of the log message.
//
// Release builds will include kCritical, kError, kWarn, and kInfo.
// Debug builds will additionally include kDebug and kTrace.
enum class LogLevel {
  kCritical, // Serious errors that are not recoverable.
  kError,    // Serious errors that are recoverable.
  kWarn,     // Warning messages.
  kInfo,     // Informational messages.
  kDebug,    // Extra messages to help with debugging.
  kTrace,    // Exact steps performed in a program (might flood the log).
};

// kMaxLogLevel specifies which log severity should be printed at compile time
// based on the build type.
inline constexpr LogLevel kMaxLogLevel =
    is_debug_build() ? LogLevel::kTrace : LogLevel::kInfo;

// should_log returns true if the log message should be enabled at compile time.
constexpr bool
should_log(LogLevel level)
{
  return level <= kMaxLogLevel;
}

// Logger provides an interface for custom logging sinks. This allows for
// customization of both the format in which logs are printed and the output
// they are printed to.
class Logger {
public:
  // available just in case a Logger wants to do some teardown.
  virtual ~Logger() = default;

  // vlog appends a log message.
  virtual void vlog(LogLevel level, std::string_view fmt,
                    const std::source_location &loc,
                    std::format_args &&args) = 0;
  // flush forces writes to complete for any pending log messages.
  virtual void flush() = 0;
};

// ConsoleLogger prints formatted log messages to stderr.
class ConsoleLogger : public Logger {
public:
  ConsoleLogger(LogLevel level = is_debug_build() ? LogLevel::kDebug
                                                  : LogLevel::kInfo)
    : max_level_(level)
  {}

  void vlog(LogLevel level, std::string_view fmt,
            const std::source_location &loc, std::format_args &&args) override;
  void flush() override;

  // set_log_level adjusts the maximum severity level to print at runtime
  void set_log_level(LogLevel level) { max_level_ = level; }

private:
  LogLevel max_level_;
};

namespace detail {

extern std::unique_ptr<Logger> logger;

template<LogLevel Level>
void
do_log(std::string_view fmt, const std::source_location &loc,
       std::format_args &&args)
{
  if constexpr (should_log(Level)) {
    logger->vlog(Level, fmt, loc, std::move(args));
  }
}

} // namespace detail

// NOLINTBEGIN(readability-identifier-naming)

// log appends a log message with a severity level determined at runtime.
template<typename... Args> struct log {
  log(LogLevel level, std::format_string<Args...> fmt, Args &&...args,
      const std::source_location &loc = std::source_location::current())
  {
    if (level > kMaxLogLevel)
      return;

    detail::logger->vlog(level, fmt.get(), loc, std::make_format_args(args...));
  }
};

template<typename... Args>
log(LogLevel, std::format_string<Args...>, Args &&...) -> log<Args...>;

// panic appends a critical log message and terminates the program.
template<typename... Args> struct panic {
  [[noreturn]] panic(
      std::format_string<Args...> fmt, Args &&...args,
      const std::source_location &loc = std::source_location::current())
  {
    detail::do_log<LogLevel::kCritical>(fmt.get(), loc,
                                        std::make_format_args(args...));
    detail::logger->flush();
    // TODO(amb): do more to cleanup safely (e.g., flush to disk?)
    std::terminate();
  }
};

template<typename... Args>
panic(LogLevel, std::format_string<Args...>, Args &&...) -> panic<Args...>;

//
// Convenient logging functions for each severity level.
//
// These versions will automatically compile out depending on the build type.
//
// Example:
// warn("foo={}, too large", foo);
//

#define MAKE_LOG_FUNC(func, level)                                             \
  template<typename... Args> struct func {                                     \
    func(std::format_string<Args...> fmt, Args &&...args,                      \
         const std::source_location &loc = std::source_location::current())    \
    {                                                                          \
      detail::do_log<LogLevel::level>(fmt.get(), loc,                          \
                                      std::make_format_args(args...));         \
    }                                                                          \
  };                                                                           \
  template<typename... Args>                                                   \
  func(std::format_string<Args...>, Args &&...)->func<Args...>

MAKE_LOG_FUNC(critical, kCritical);
MAKE_LOG_FUNC(error, kError);
MAKE_LOG_FUNC(warn, kWarn);
MAKE_LOG_FUNC(info, kInfo);
MAKE_LOG_FUNC(debug, kDebug);
MAKE_LOG_FUNC(trace, kTrace);

#undef MAKE_LOG_FUNC

// NOLINTEND(readability-identifier-naming)

// log_init initializes the logging subsystem.
//
// logger can be whatever type of logging sink you would like. This function
// must be called before using logging.
inline void
log_init(std::unique_ptr<Logger> logger)
{
  detail::logger = std::move(logger);
}

} // namespace hogd
