#include "hogd/test_ffi/logger.h"

#include <format>
#include <source_location>
#include <string_view>

#include "hogd/base/log.h"

namespace hogd {

void
PerThreadLogger::vlog(LogLevel level, std::string_view fmt,
                      const std::source_location &loc, std::format_args args)
{
  logger_->vlog(level, fmt, loc, args);
}

void
PerThreadLogger::flush()
{
  logger_->flush();
}

} // namespace hogd
