#include "hogd/metrics/heap_profile.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hogd {
namespace {
using Number = std::uint64_t;

void
varint(std::string &out, Number value)
{
  while (value >= 128) {
    out += static_cast<char>((value & 127) | 128);
    value >>= 7;
  }
  out += static_cast<char>(value);
}

void
integer(std::string &out, Number field, Number value)
{
  varint(out, field << 3);
  varint(out, value);
}

void
message(std::string &out, Number field, std::string_view value)
{
  varint(out, (field << 3) | 2);
  varint(out, value.size());
  out += value;
}

Number
number(const std::string &text, int base = 10)
{
  if (text.empty() || text.front() == '-')
    throw std::invalid_argument("invalid heap profile number");
  std::size_t end = 0;
  Number value = std::stoull(text, &end, base);
  if (end != text.size())
    throw std::invalid_argument("invalid heap profile number");
  return value;
}

struct Mapping {
  Number start, limit, offset, filename;
};

Number
adjusted(Number count, Number bytes, Number period)
{
  if (!count || !bytes)
    return 0;
  const double probability =
      -std::expm1(-static_cast<double>(bytes) / static_cast<double>(count) /
                  static_cast<double>(period));
  const double value = static_cast<double>(bytes) / probability;
  if (!std::isfinite(value) ||
      value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("heap profile sample overflow");
  return static_cast<Number>(std::round(value));
}
} // namespace

std::string
jemalloc_to_pprof(std::string_view dump, std::string_view maps)
{
  std::istringstream input{std::string(dump)};
  std::string line;
  if (!std::getline(input, line) || !line.starts_with("heap_v2/"))
    throw std::invalid_argument("expected jemalloc heap_v2");
  const Number period = number(line.substr(8));
  if (!period)
    throw std::invalid_argument("zero heap sampling period");

  std::vector<std::string> strings{"", "inuse_objects", "count", "inuse_space",
                                   "bytes"};
  std::vector<Mapping> mappings;
  std::istringstream map_input{std::string(maps)};
  while (std::getline(map_input, line)) {
    std::istringstream fields(line);
    std::string range, permissions, offset, device, inode, filename;
    if (!(fields >> range >> permissions >> offset >> device >> inode))
      throw std::invalid_argument("invalid proc maps entry");
    const auto dash = range.find('-');
    if (dash == std::string::npos)
      throw std::invalid_argument("invalid proc maps range");
    std::getline(fields >> std::ws, filename);
    Number start = number(range.substr(0, dash), 16);
    Number limit = number(range.substr(dash + 1), 16);
    if (start >= limit)
      throw std::invalid_argument("invalid proc maps limits");
    strings.push_back(filename);
    mappings.push_back({start, limit, number(offset, 16), strings.size() - 1});
  }

  std::string profile;
  for (auto [type, unit] : {std::pair{1, 2}, std::pair{3, 4}}) {
    std::string value_type;
    integer(value_type, 1, type);
    integer(value_type, 2, unit);
    message(profile, 1, value_type);
  }
  std::map<Number, Number> locations;
  std::vector<Number> stack;
  bool consumed = true;
  while (std::getline(input, line)) {
    if (line == "MAPPED_LIBRARIES:")
      break;
    if (line.starts_with("@ ")) {
      if (!consumed)
        throw std::invalid_argument("stack without aggregate");
      stack.clear();
      consumed = false;
      std::istringstream frames(line.substr(2));
      std::string address;
      while (frames >> address) {
        // Match jeprof: the leaf is exact; caller PCs are return addresses.
        Number pc = number(address, 16);
        if (!pc)
          throw std::invalid_argument("zero frame address");
        if (!stack.empty())
          --pc;
        auto [it, inserted] = locations.try_emplace(pc, locations.size() + 1);
        stack.push_back(it->second);
      }
      if (stack.empty())
        throw std::invalid_argument("empty heap stack");
    }
    else {
      std::istringstream fields(line);
      std::string tag, count, bytes, cumulative_count, cumulative_bytes;
      if (!(fields >> tag) || tag != "t*:")
        continue; // Per-thread rows and the header summary are not samples.
      if (stack.empty())
        continue;
      if (consumed ||
          !(fields >> count >> bytes >> cumulative_count >> cumulative_bytes) ||
          !count.ends_with(':') || !cumulative_count.starts_with('[') ||
          !cumulative_count.ends_with(':') || !cumulative_bytes.ends_with(']'))
        throw std::invalid_argument("invalid heap aggregate");
      consumed = true;
      Number n = number(count.substr(0, count.size() - 1));
      Number b = number(bytes);
      number(cumulative_count.substr(1, cumulative_count.size() - 2));
      number(cumulative_bytes.substr(0, cumulative_bytes.size() - 1));
      if ((n == 0) != (b == 0))
        throw std::invalid_argument("inconsistent heap sample");
      if (!n)
        continue;
      Number scaled_bytes = adjusted(n, b, period);
      Number scaled_count = static_cast<Number>(
          std::round(static_cast<double>(scaled_bytes) *
                     static_cast<double>(n) / static_cast<double>(b)));
      std::string sample;
      for (Number id : stack)
        integer(sample, 1, id);
      integer(sample, 2, scaled_count);
      integer(sample, 2, scaled_bytes);
      message(profile, 2, sample);
    }
  }
  if (!consumed)
    throw std::invalid_argument("stack without aggregate");
  for (std::size_t i = 0; i < mappings.size(); ++i) {
    const auto &m = mappings[i];
    std::string encoded;
    integer(encoded, 1, i + 1);
    integer(encoded, 2, m.start);
    integer(encoded, 3, m.limit);
    integer(encoded, 4, m.offset);
    integer(encoded, 5, m.filename);
    message(profile, 3, encoded);
  }
  for (auto [pc, id] : locations) {
    std::string location;
    integer(location, 1, id);
    for (std::size_t i = 0; i < mappings.size(); ++i) {
      if (pc >= mappings[i].start && pc < mappings[i].limit) {
        integer(location, 2, i + 1);
        break;
      }
    }
    integer(location, 3, pc);
    message(profile, 4, location);
  }
  for (const auto &value : strings)
    message(profile, 6, value);
  integer(profile, 14, 3); // Default sample type: inuse_space.
  return profile;
}
} // namespace hogd
