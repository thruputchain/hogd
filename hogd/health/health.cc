#include "hogd/health/health.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "hogd/base/json.h"
#include "hogd/http/http.h"
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
write_file_atomically(const std::filesystem::path &path,
                      const std::string &contents)
{
  // Create the parent directories in [path] if they do not already exist.
  std::error_code ec;
  if (const std::filesystem::path parent_path = path.parent_path();
      !parent_path.empty()) {
    std::filesystem::create_directories(parent_path, ec);
    CHECK(!ec);
  }

  const std::filesystem::path temp_path = make_temp_path(path);
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

  ec.clear();
  std::filesystem::rename(temp_path, path, ec);
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
Health::dump_to_file(std::optional<std::filesystem::path> opt_path)
{
  std::string buffer = co_await dump_to_string();

  std::filesystem::path path;
  if (opt_path.has_value()) {
    path = *opt_path;
  }
  else {
    path = std::filesystem::path(CMAKE_ROOT_BINARY_DIR);
    path = path / "health";
    path = path / std::to_string(pid_);
  }
  write_file_atomically(path, buffer);

  co_return;
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

void
add_health_metrics_routes(HttpServer &server)
{
  server.add_route("/health", []() -> x23::Async<HttpResponse> {
    co_return HttpResponse{
        .status = 200,
        .content_type = "application/json",
        .body = co_await Health::instance().dump_to_string(),
    };
  });
  server.add_route("/metrics", [] {
    return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = "",
    };
  });
}

} // namespace hogd
