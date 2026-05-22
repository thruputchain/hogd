#include "hogd/health/health.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
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
namespace {

std::filesystem::path
make_temp_path(const std::filesystem::path &path)
{
  static std::atomic<std::uint64_t> next_temp_file_id{0};
  std::filesystem::path temp_path = path;
  temp_path += std::format(".tmp.{}.{}", getpid(), next_temp_file_id++);
  return temp_path;
}

void
write_file_atomically(const std::string &path, const std::string &contents)
{
  const std::filesystem::path final_path(path);
  const std::filesystem::path temp_path = make_temp_path(final_path);

  {
    std::ofstream out(temp_path);
    if (!out) {
      x23::panic("failed to open health temp file {}", temp_path.string());
    }
    out << contents << '\n';
    if (!out) {
      x23::panic("failed to write health temp file {}", temp_path.string());
    }
  }

  std::error_code ec;
  std::filesystem::rename(temp_path, final_path, ec);
  CHECK(!ec);
}

} // namespace

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

  write_file_atomically(path, buffer);
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
