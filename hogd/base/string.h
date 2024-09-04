// string.h - utilities for string manipulation

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

// split creates a vector of string views delineated by a seperator character
inline std::vector<std::string_view>
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

} // namespace hogd
