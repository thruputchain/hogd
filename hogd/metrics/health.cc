#include "hogd/metrics/health.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "hogd/base/json.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

#include <unistd.h>

#include "hogd/base/log.h"
#include "x23/debug.h"

namespace hogd {

Health &
Health::instance()
{
  static Health health;
  return health;
}

x23::Async<std::string>
Health::dump_to_string()
{
  x23::CoroGuard g;
  co_await g(lock_);

  update_timestamp();

  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("process_name", process_name_);
    w.field("pid", pid_);
    w.field("executable_path", executable_path_);
    w.field("last_updated_timestamp", last_updated_timestamp_.format());
    w.field("status", std::format("{}", status_));
    w.field("current_block_height",
            static_cast<std::int64_t>(current_block_height_));
  });

  co_return buffer;
}

x23::Async<void>
Health::set_status(Status status)
{
  x23::CoroGuard g;
  co_await g(lock_);

  status_ = status;

  co_await g.reset_and_return();
}

x23::Async<void>
Health::set_current_block_height(BlockHeight current_block_height)
{
  x23::CoroGuard g;
  co_await g(lock_);

  current_block_height_ = current_block_height;

  co_await g.reset_and_return();
}

} // namespace hogd
