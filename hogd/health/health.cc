#include "hogd/health/health.h"

#include <format>
#include <fstream>

namespace hogd {

void
Health::dump_to_file(const char *path) const
{
  std::ofstream out(path);
  out << std::format("{:%Y-%m-%d %H:%M:%S}", last_updated_timestamp_) << '\n';
}

} // namespace hogd
