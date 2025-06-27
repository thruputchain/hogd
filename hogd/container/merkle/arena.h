// arena.h - provides an arena for allocating memory.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include <format>
#include <iomanip>
#include <ios>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

class Arena {
public:
  Arena() = default;

  // Disable copy and move.
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) noexcept = delete;
  Arena &operator=(Arena &&) noexcept = delete;
};

} // namespace hogd
