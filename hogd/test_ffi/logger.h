#pragma once

#include <atomic>
#include <format>
#include <source_location>
#include <string_view>

#include "hogd/base/log.h"

namespace hogd {

class ThreadsafeLogger : public Logger {
public:
  ThreadsafeLogger(LogLevel log_level) : max_level_(log_level) {}

  ~ThreadsafeLogger() override = default;

  void vlog(LogLevel level, std::string_view fmt,
            const std::source_location &loc, std::format_args args) override;
  void flush() override;

  LogLevel get_max_log_level() const override { return max_level_.load(); }

  void set_log_level(LogLevel level) { max_level_.store(level); }

private:
  std::atomic<LogLevel> max_level_;
};

} // namespace hogd
