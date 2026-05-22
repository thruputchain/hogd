#pragma once

#include <chrono>
#include <format>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

class Health {
public:
  enum class Status {
    kOk,
  };

  ~Health() = default;

  // Returns the [Health] singleton instance.
  static Health &instance();

  void dump_to_file(const std::string &path) const;

  // Disable copy and move.
  Health(const Health &) = delete;
  Health &operator=(const Health &) = delete;
  Health(Health &&) = delete;
  Health &operator=(Health &&) = delete;

private:
  // [Health] should only be used via a singleton instance.
  Health() = default;

  struct Timestamp {
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();

    friend struct std::formatter<Timestamp>;
  };

  Status status_ = Status::kOk;
  Timestamp last_updated_timestamp_;
  BlockHeight current_block_height_ = 0;
};

} // namespace hogd

template<>
struct std::formatter<hogd::Health::Status> : std::formatter<const char *> {
  template<typename FormatContext>
  auto format(hogd::Health::Status status, FormatContext &ctx) const
  {
    switch (status) {
    case hogd::Health::Status::kOk:
      return std::formatter<const char *>::format("ok", ctx);
    }
    __builtin_unreachable();
  }
};

template<>
struct std::formatter<hogd::Health::Timestamp>
  : std::formatter<std::chrono::system_clock::time_point> {
  template<typename FormatContext>
  auto format(const hogd::Health::Timestamp &timestamp,
              FormatContext &ctx) const
  {
    return std::formatter<std::chrono::system_clock::time_point>::format(
        timestamp.timestamp, ctx);
  }
};
