#pragma once

#include <chrono>
#include <format>
#include <string>
#include <thread>

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
// Call this to start the health service. Some executables, such as tests, will
// not want to run the health service.
// Health::instance().start_service();
//
// Update the health status:
// Health::instance().set_status(Health::Status::kOk);
// Health::instance().set_current_block_height(11);
// ...
//
// You can manually dump the current health status to disk with
// [Health::instance().dump_to_file()], but there should be generally no need to
// call this as the Health service maintains a background thread that dumps to
// the file periodically.
class Health {
public:
  enum class Status {
    kStarting,
    kOk,
    kError,
  };

  ~Health()
  {
    should_exit_ = true;

    // There will be no worker thread if the health service was not started.
    if (worker_thread_) {
      worker_thread_->join();
    }
  }

  // Returns the [Health] singleton instance.
  static Health &instance();

  // Start the health service.
  void start_service()
  {
    instance().worker_thread_ = std::thread(&Health::worker, this);
  }

  x23::Async<void> dump_to_file(
      std::optional<std::filesystem::path> opt_path = std::nullopt) const;

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

  void worker()
  {
    while (!should_exit_) {
      dump_to_file()();
      std::this_thread::sleep_for(kDefaultWritePeriod);
    }
  }

  void update_timestamp()
  {
    CHECK(lock_.busy());
    last_updated_timestamp_.timestamp = std::chrono::system_clock::now();
  }

  std::atomic<bool> should_exit_ = false;
  std::optional<std::thread> worker_thread_ = std::nullopt;
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
