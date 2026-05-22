#include "hogd/health/health.h"

#include "hogd/base/json.h"

#include <format>
#include <fstream>
#include <string>

namespace hogd {

Health &
Health::instance()
{
  static Health health;
  return health;
}

void
Health::dump_to_file(const std::string &path) const
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("last_updated_timestamp",
            std::format("{:%Y-%m-%d %H:%M:%S}", last_updated_timestamp_));
    w.field("status", std::format("{}", status_));
    w.field("current_block_height",
            static_cast<std::int64_t>(current_block_height_));
  });

  std::ofstream out(path);
  out << buffer << '\n';
}

} // namespace hogd
