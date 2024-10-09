// log.h - logging infrastructure

#pragma once

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <source_location>

#include "hogd/base/compiler.h"
#include "hogd/base/sync.h"

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

[[noreturn]] void do_panic(std::string_view fmt,
                           const std::source_location &loc,
                           std::format_args &&args);

template<typename Tag>
bool
should_log_first_n(int n)
{
  static LossyCounter<int> c;
  if (c >= n)
    return false;
  c.inc();
  return true;
}

template<typename Tag>
bool
should_log_every_n(int n)
{
  static LossyCounter<uint32_t> c;
  return c.inc() % static_cast<uint32_t>(n) == 0;
}

template<typename Tag>
bool
should_log_every_n_sec(float n)
{
  // check if timeout has expired
  int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  static std::atomic<int64_t> next_ts{0};
  int64_t next = next_ts.load(std::memory_order_relaxed);
  if (now < next)
    return false;

  // timeout has expired; update next_ts
  while (true) {
    if (next_ts.compare_exchange_weak(next, now + static_cast<int64_t>(n * 1e6),
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      break;
    }
  }
  return true;
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
    detail::do_panic(fmt.get(), loc, std::make_format_args(args...));
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

//
// Convenient logging functions for each severity level with extended
// functionality.
//
// These versions will also automatically compile out depending on the build
// type.
//
// Example:
// warn_first_n(1, "foo={}, too large", foo); // only warns once
//

#define MAKE_LOG_FUNC_EX(func, should_type, should_fn, level)                  \
  template<typename Tag, typename... Args> struct func {                       \
    func(should_type n, std::format_string<Args...> fmt, Args &&...args,       \
         const std::source_location &loc = std::source_location::current())    \
    {                                                                          \
      if constexpr (LogLevel::level <= kMaxLogLevel) {                         \
        if (!detail::should_log_##should_fn<Tag>(n))                           \
          return;                                                              \
        detail::logger->vlog(LogLevel::level, fmt.get(), loc,                  \
                             std::make_format_args(args...));                  \
      }                                                                        \
    }                                                                          \
  };                                                                           \
  template<typename Tag = decltype([] {}), typename... Args>                   \
  func(should_type n, std::format_string<Args...>, Args &&...)                 \
      ->func<Tag, Args...>

// first_n - stops logging after n invocations at the call site
MAKE_LOG_FUNC_EX(critical_first_n, int, first_n, kCritical);
MAKE_LOG_FUNC_EX(error_first_n, int, first_n, kError);
MAKE_LOG_FUNC_EX(warn_first_n, int, first_n, kWarn);
MAKE_LOG_FUNC_EX(info_first_n, int, first_n, kInfo);
MAKE_LOG_FUNC_EX(debug_first_n, int, first_n, kDebug);
MAKE_LOG_FUNC_EX(trace_first_n, int, first_n, kTrace);

// every_n - logs once after each n invocations at the call site
MAKE_LOG_FUNC_EX(critical_every_n, int, every_n, kCritical);
MAKE_LOG_FUNC_EX(error_every_n, int, every_n, kError);
MAKE_LOG_FUNC_EX(warn_every_n, int, every_n, kWarn);
MAKE_LOG_FUNC_EX(info_every_n, int, every_n, kInfo);
MAKE_LOG_FUNC_EX(debug_every_n, int, every_n, kDebug);
MAKE_LOG_FUNC_EX(trace_every_n, int, every_n, kTrace);

// every_n_sec - logs after at least n seconds have elapsed at the call site
MAKE_LOG_FUNC_EX(critical_every_n_sec, float, every_n_sec, kCritical);
MAKE_LOG_FUNC_EX(error_every_n_sec, float, every_n_sec, kError);
MAKE_LOG_FUNC_EX(warn_every_n_sec, float, every_n_sec, kWarn);
MAKE_LOG_FUNC_EX(info_every_n_sec, float, every_n_sec, kInfo);
MAKE_LOG_FUNC_EX(debug_every_n_sec, float, every_n_sec, kDebug);
MAKE_LOG_FUNC_EX(trace_every_n_sec, float, every_n_sec, kTrace);

#undef MAKE_LOG_FUNC_EX

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
