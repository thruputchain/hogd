#pragma once

#include <array>
#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

class AliasGuard {
public:
  // Returns false if [ptr, ptr+len) overlaps any previously registered range.
  [[nodiscard]] bool check_and_register(const void *ptr, size_t len) noexcept
  {
    auto start = reinterpret_cast<uintptr_t>(ptr);
    auto end = start + len;
    for (size_t i = 0; i < count_; ++i) {
      if (start < ranges_[i].end && ranges_[i].start < end) {
        return false;
      }
    }

    CHECK(count_ < kMaxRanges);
    ranges_[count_++] = {.start = start, .end = end};
    return true;
  }

private:
  struct Range {
    uintptr_t start;
    uintptr_t end;
  };
  static constexpr size_t kMaxRanges = 8;
  std::array<Range, kMaxRanges> ranges_{};
  size_t count_ = 0;
};

} // namespace hogd
