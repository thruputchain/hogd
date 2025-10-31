#pragma once

#include <format>
#include <source_location>
#include <string_view>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"

namespace hogd {

class PerThreadLogger : public Logger {
public:
  PerThreadLogger() = default;
  ~PerThreadLogger() = default;

  void vlog(LogLevel level, std::string_view fmt,
            const std::source_location &loc, std::format_args args) override;
  void flush() override;

  LogLevel get_max_log_level() const override
  {
    return logger_->get_max_log_level();
  }

  void set_log_level(LogLevel level) { logger_->set_log_level(level); }

private:
  PerThread<ConsoleLogger> logger_;
};

} // namespace hogd
