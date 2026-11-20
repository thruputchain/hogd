#pragma once

#include <array>
#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

// Checks the memory arguments of one syscall against each other. Every
// argument that names memory is registered as an input, which the host only
// reads, or as an output, which it writes.
//
// An output may not overlap any other argument. Inputs may overlap each
// other: nothing is written through either, so no result depends on how they
// are laid out.
//
// Two ranges overlap under the half-open predicate
// (a.start < b.end && b.start < a.end), so a zero-length range overlaps a
// range it lies strictly inside, and not one it only touches at an edge.
//
// A guest reaches all of this through any syscall, so it is
// consensus-observable.
class AliasGuard {
public:
  // Returns false if [start, start+len) overlaps a previously registered
  // output.
  bool check_and_register_input(uintptr_t start, size_t len) noexcept
  {
    return check_and_register(start, len, Use::kInput);
  }

  // Returns false if [start, start+len) overlaps any previously registered
  // range.
  bool check_and_register_output(uintptr_t start, size_t len) noexcept
  {
    return check_and_register(start, len, Use::kOutput);
  }

  // Whether every registration so far returned true.
  bool check() const { return no_aliasing_violation_; }

private:
  enum class Use : uint8_t { kInput, kOutput };

  bool check_and_register(uintptr_t start, size_t len, Use use) noexcept
  {
    auto end = start + len;
    for (size_t i = 0; i < count_; ++i) {
      if (use == Use::kInput && ranges_[i].use == Use::kInput) {
        continue;
      }
      if (start < ranges_[i].end && ranges_[i].start < end) {
        no_aliasing_violation_ = false;
        return false;
      }
    }

    CHECK(count_ < kMaxRanges);
    ranges_[count_++] = {.start = start, .end = end, .use = use};
    return true;
  }

  struct Range {
    uintptr_t start;
    uintptr_t end;
    Use use;
  };
  // A syscall has seven argument words, so at most seven memory arguments.
  static constexpr size_t kMaxRanges = 8;
  std::array<Range, kMaxRanges> ranges_{};
  size_t count_ = 0;
  bool no_aliasing_violation_ = true;
};

} // namespace hogd
