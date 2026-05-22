#pragma once

#include <chrono>
#include <format>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class Health {
public:
  enum class Status {
    kOk,
    kError,
  };

  ~Health() = default;

  // Returns the [Health] singleton instance.
  static Health &instance();

  x23::Async<void> dump_to_file(const std::string &path) const;

  x23::Async<void> set_status(Status status);

  x23::Async<void> set_current_block_height(BlockHeight current_block_height);

  // Disable copy and move.
  Health(const Health &) = delete;
  Health &operator=(const Health &) = delete;
  Health(Health &&) = delete;
  Health &operator=(Health &&) = delete;

private:
  struct Timestamp {
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();

    friend struct std::formatter<Timestamp>;
  };

  // [Health] should only be used via a singleton instance.
  Health() = default;

  void update_timestamp()
  {
    CHECK(lock_.busy());
    last_updated_timestamp_.timestamp = std::chrono::system_clock::now();
  }

  mutable x23::CoroLock lock_;
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
    case hogd::Health::Status::kError:
      return std::formatter<const char *>::format("error", ctx);
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
