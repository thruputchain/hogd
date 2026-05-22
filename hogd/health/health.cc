#include "hogd/health/health.h"

#include <format>
#include <fstream>
#include <string>

#include "hogd/base/json.h"
#include "hogd/base/types.h"

namespace hogd {

Health &
Health::instance()
{
  static Health health;
  return health;
}

x23::Async<void>
Health::dump_to_file(const std::string &path) const
{
  x23::CoroGuard g;
  co_await g(lock_);

  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("last_updated_timestamp", last_updated_timestamp_.format());
    w.field("status", std::format("{}", status_));
    w.field("current_block_height",
            static_cast<std::int64_t>(current_block_height_));
  });

  g.reset();

  std::ofstream out(path);
  out << buffer << '\n';
  co_return;
}

x23::Async<void>
Health::set_status(Status status)
{
  x23::CoroGuard g;
  co_await g(lock_);

  update_timestamp();
  status_ = status;

  co_await g.reset_and_return();
}

x23::Async<void>
Health::set_current_block_height(BlockHeight current_block_height)
{
  x23::CoroGuard g;
  co_await g(lock_);

  update_timestamp();
  current_block_height_ = current_block_height;

  co_await g.reset_and_return();
}

} // namespace hogd
