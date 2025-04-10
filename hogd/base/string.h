// string.h - utilities for string manipulation

#pragma once

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

// split creates a vector of string views delineated by a seperator character
constexpr std::vector<std::string_view>
split(std::string_view text, char sep = ' ',
      std::optional<size_t> max = std::nullopt)
{
  std::vector<std::string_view> tokens;
  std::string_view::size_type start = 0, end;

  // segment tokens based on seperator character
  while ((end = text.find(sep, start)) != std::string_view::npos) {
    tokens.push_back(text.substr(start, end - start));
    start = end + 1;
    if (max && tokens.size() >= *max)
      break;
  }

  // push the remaining token; or the rest of text if the limit was hit
  tokens.push_back(text.substr(start));
  return tokens;
}

// print_iec_bytes converts an integer into IEC units.
std::string print_iec_bytes(size_t val, bool add_space = false);
// print_si_bytes converts an integer into SI units.
std::string print_si_bytes(size_t val, bool add_space = false);

// parse_integer transforms a string into an integer type. It returns
// std::nullopt if parsing failed.
template<std::integral T>
constexpr std::optional<T>
parse_integer(std::string_view str)
{
  T ret;
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), ret);
  if (ec != std::errc())
    return std::nullopt;
  return ret;
}

} // namespace hogd
