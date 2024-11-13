#include "hogd/base/string.h"

#include <format>
#include <string>
#include <utility>

#include "hogd/base/types.h"

namespace hogd {

namespace {

using Unit = std::pair<std::string, size_t>;

const Unit iec_byte_units[] = {
    {"PiB", 1UL << 50}, {"TiB", 1UL << 40}, {"GiB", 1UL << 30},
    {"MiB", 1UL << 20}, {"KiB", 1UL << 10}, {"B", 0},
};

const Unit si_byte_units[] = {
    {"PB", 1'000'000'000'000'000},
    {"TB", 1'000'000'000'000},
    {"GB", 1'000'000'000},
    {"MB", 1'000'000},
    {"KB", 1'000},
    {"B", 0},
};

template<size_t N>
std::string
pretty_print(const Unit (&tbl)[N], size_t val, bool add_space)
{
  for (const auto &[name, size] : tbl) {
    if (val < size)
      continue;
    return std::format("{:.4g}{}{}",
                       static_cast<double>(val) / static_cast<double>(size),
                       add_space ? " " : "", name);
  }

  return add_space ? "0 B" : "0B";
}

} // namespace

std::string
print_iec_bytes(size_t val, bool add_space)
{
  return pretty_print(iec_byte_units, val, add_space);
}

std::string
print_si_bytes(size_t val, bool add_space)
{
  return pretty_print(si_byte_units, val, add_space);
}

} // namespace hogd
