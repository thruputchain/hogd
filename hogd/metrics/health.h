#pragma once

#include <chrono>
#include <format>
#include <string>
#include <thread>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/progname.h"

namespace hogd {

// Writes the current health status of the process to a file of disk on a
// periodic basis (default = 1 second). This file is hosted with a web service
// at the [/health] URL.
//
// Example:
// Health::instance().set_status(Health::Status::kOk);
// Health::instance().set_current_block_height(11);
// ...
//
// The health status is hosted via the http service in [hogd/http/], which calls
// [dump_to_string()] in this class.
class Health {
public:
  enum class Status {
    kStarting,
    kOk,
    kError,
  };

  // Returns the [Health] singleton instance.
  static Health &instance();

  x23::Async<std::string> dump_to_string();

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

    std::string format() const
    {
      return std::format("{:%Y-%m-%dT%H:%M:%SZ}", timestamp);
    }
  };

  static constexpr std::chrono::seconds kDefaultWritePeriod =
      std::chrono::seconds(1);

  // [Health] should only be used via the singleton instance.
  Health()
  {
    process_name_ = read_process_name();
    pid_ = getpid();
    executable_path_ = x23::getprogpath();
    last_updated_timestamp_.timestamp = std::chrono::system_clock::now();
  }

  void update_timestamp()
  {
    CHECK(lock_.busy());
    last_updated_timestamp_.timestamp = std::chrono::system_clock::now();
  }

  mutable x23::CoroLock lock_;

  std::string process_name_;
  pid_t pid_;
  std::string executable_path_;
  Status status_ = Status::kStarting;
  Timestamp last_updated_timestamp_;
  BlockHeight current_block_height_ = -1;
};

} // namespace hogd

template<>
struct std::formatter<hogd::Health::Status> : std::formatter<const char *> {
  template<typename FormatContext>
  auto format(hogd::Health::Status status, FormatContext &ctx) const
  {
    switch (status) {
    case hogd::Health::Status::kStarting:
      return std::formatter<const char *>::format("starting", ctx);
    case hogd::Health::Status::kOk:
      return std::formatter<const char *>::format("ok", ctx);
    case hogd::Health::Status::kError:
      return std::formatter<const char *>::format("error", ctx);
    }
    __builtin_unreachable();
  }
};
