#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

#include "hogd/config.h"

#if HAVE_CXX_STACKTRACE
#include <stacktrace>
#endif // HAVE_CXX_STACKTRACE

#include "hogd/base/ansi_color.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"

namespace hogd {

namespace detail {

// logger is the logging implementation currently in use.
std::unique_ptr<Logger> logger;

[[noreturn]] void
do_panic(std::string_view fmt, const std::source_location &loc,
         std::format_args args)
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

namespace {

#ifndef NDEBUG
thread_local bool log_in_progress = false;

[[noreturn]] void
abort_recursive_log() noexcept
{
  constexpr char message[] =
      "recursive logging detected; ConsoleLogger formatters must not log\n";
  (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
  std::abort();
}

class LogRecursionGuard {
public:
  LogRecursionGuard() noexcept
  {
    if (log_in_progress)
      abort_recursive_log();
    log_in_progress = true;
  }

  ~LogRecursionGuard() noexcept { log_in_progress = false; }

  LogRecursionGuard(const LogRecursionGuard &) = delete;
  LogRecursionGuard &operator=(const LogRecursionGuard &) = delete;
};
#endif // NDEBUG

constexpr std::size_t kLogBufferSize = 16 * 1024;
constexpr std::size_t kLogSuffixSize = 4; // "...\n"

thread_local std::array<char, kLogBufferSize> log_buffer;

class TruncatingOutputIterator {
public:
  using difference_type = std::ptrdiff_t;

  TruncatingOutputIterator(char *&out, char *end, bool &truncated) noexcept
    : out_(&out), end_(end), truncated_(&truncated)
  {}

  TruncatingOutputIterator &operator*() noexcept { return *this; }
  TruncatingOutputIterator &operator++() noexcept { return *this; }
  TruncatingOutputIterator operator++(int) noexcept { return *this; }

  TruncatingOutputIterator &operator=(char c) noexcept
  {
    if (*out_ != end_) {
      **out_ = c;
      ++*out_;
    }
    else {
      *truncated_ = true;
    }
    return *this;
  }

private:
  char **out_;
  char *end_;
  bool *truncated_;
};

void
write_stderr(std::string_view line) noexcept
{
  int saved_errno = errno;
  ::flockfile(stderr);
  while (!line.empty()) {
    ssize_t written = ::write(STDERR_FILENO, line.data(), line.size());
    if (written > 0) {
      line.remove_prefix(static_cast<std::size_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    break;
  }
  ::funlockfile(stderr);
  errno = saved_errno;
}

constexpr std::string_view
log_level_to_string(LogLevel level)
{
  using enum LogLevel;
  switch (level) {
  case kCritical:
    return "critical";
  case kError:
    return "error";
  case kWarn:
    return "warn";
  case kInfo:
    return "info";
  case kDebug:
    return "debug";
  case kTrace:
    return "trace";
  default:
    return "unknown";
  }
}

std::string_view
source_target(std::string_view path)
{
  constexpr std::string_view kHogdPrefix = "hogd/";

  std::size_t start = path.rfind(kHogdPrefix);
  if (start != std::string_view::npos) {
    start += kHogdPrefix.size();
  }
  else {
    start = 0;
  }

  // Build-tree and installed paths differ, so derive the subsystem from the
  // source path suffix under `hogd/` instead of requiring an absolute prefix.
  std::size_t end = path.find('/', start);
  std::string_view target = end == std::string_view::npos
                                ? path.substr(start)
                                : path.substr(start, end - start);
  if (target.ends_with(".cc") || target.ends_with(".h")) {
    return "hogd";
  }
  return target.empty() ? std::string_view("hogd") : target;
}

} // namespace

void
ConsoleLogger::vlog(LogLevel level, std::string_view fmt,
                    const std::source_location &loc, std::format_args args)
{
  // check if the severity is high enough
  if (level > max_level_)
    return;

#ifndef NDEBUG
  LogRecursionGuard recursion_guard;
#endif

  // record the current timestamp
  const auto now = std::chrono::system_clock::now();

  // extract the filename
  std::string_view filename = detail::source_basename(loc.file_name());

  char *out = log_buffer.data();
  char *end = log_buffer.data() + log_buffer.size() - kLogSuffixSize;
  bool truncated = false;
  std::format_to(TruncatingOutputIterator(out, end, truncated),
                 "\e[1m{}\e[0;34m[{:%m%d %H:%M:%OS} {}:{}]\e[0m ",
                 log_level_to_char(level), now, filename, loc.line());
  std::vformat_to(TruncatingOutputIterator(out, end, truncated), fmt, args);
  if (truncated) {
    *out++ = '.';
    *out++ = '.';
    *out++ = '.';
  }
  *out++ = '\n';
  write_stderr(
      std::string_view(log_buffer.data(),
                       static_cast<std::size_t>(out - log_buffer.data())));
}

void
ConsoleLogger::flush()
{
  // Note: cerr doesn't buffer by default, but flush just in case it was
  // configured differently.
  std::cerr.flush();
}

void
JsonConsoleLogger::vlog(LogLevel level, std::string_view fmt,
                        const std::source_location &loc, std::format_args args)
{
  if (level > max_level_)
    return;

  const auto now = std::chrono::system_clock::now();
  const auto timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count();

  // TODO(mm): this is a JSON envelope around the existing printf-style log
  // message. Should be a structured-field logging API once real user workflows
  // identify which fields need first-class filtering.
  std::string message;
  std::vformat_to(std::back_inserter(message), fmt, args);

  std::string buf;
  json::Writer(buf).object([&](json::Writer &w) {
    w.field("timestamp_ms", static_cast<std::int64_t>(timestamp_ms));
    w.field("level", log_level_to_string(level));
    w.field("target", source_target(loc.file_name()));
    w.field("message", message);
    w.object_field("fields", [&](json::Writer &w) {
      w.field("file", detail::source_basename(loc.file_name()));
      w.field("line", static_cast<std::int64_t>(loc.line()));
    });
  });
  std::format_to(std::back_inserter(buf), "\n");

  std::osyncstream(std::cerr) << buf;
}

void
JsonConsoleLogger::flush()
{
  std::cerr.flush();
}

} // namespace hogd
